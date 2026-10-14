// Foundation: NSObject, collections, NSNumber, NSData, NSCharacterSet, NSURL,
// property lists.
#include "hle/foundation.h"

#include <climits>
#include <cmath>
#include <ctime>
#include <cstring>
#include <unordered_map>

#include "hle/nsstring.h"
#include "objc/runtime.h"

namespace foundation {

template <typename T>
struct Boxed {
    Class isa;
    T* p;
};
template <typename T>
T*& box(id o) {
    return ((Boxed<T>*)o)->p;
}

namespace {

Class g_NSObject, g_NSArray, g_NSMutableArray, g_NSDictionary, g_NSMutableDictionary, g_NSSet,
    g_NSMutableSet, g_NSEnumerator, g_NSNumber, g_NSData, g_NSMutableData, g_NSCharacterSet, g_NSURL,
    g_NSURLRequest, g_NSError, g_NSNull, g_NSUUID, g_NSValue;

// ---------------------------------------------------------------------------
// NSObject
// ---------------------------------------------------------------------------
id NSObject_alloc(Class cls, SEL) { return objc::alloc_instance(cls); }
id NSObject_new(Class cls, SEL) {
    id o = objc::send((id)cls, SEL_("alloc"));
    return objc::send(o, SEL_("init"));
}
id NSObject_cls_class(Class cls, SEL) { return (id)cls; }
id NSObject_cls_superclass(Class cls, SEL) { return (id)cls->superclass; }
id NSObject_cls_description(Class cls, SEL) { return ns::str_auto(std::string(objc::class_name(cls))); }
BOOL NSObject_cls_instancesRespond(Class cls, SEL, SEL s) { return objc::responds_to(cls, s); }
BOOL NSObject_cls_isSubclassOfClass(Class cls, SEL, Class other) { return objc::is_subclass(cls, other); }
void NSObject_cls_initialize(Class, SEL) {}

id NSObject_init(id self, SEL) { return self; }
void NSObject_dealloc(id self, SEL) { objc::dispose_instance(self); }
id NSObject_retain(id self, SEL) { return objc::retain(self); }
void NSObject_release(id self, SEL) { objc::release(self); }
id NSObject_autorelease(id self, SEL) { return objc::autorelease(self); }
NSUInteger NSObject_retainCount(id self, SEL) { return objc::retain_count(self); }
id NSObject_class(id self, SEL) { return (id)self->isa; }
id NSObject_superclass(id self, SEL) { return (id)self->isa->superclass; }
id NSObject_self(id self, SEL) { return self; }
BOOL NSObject_isKindOfClass(id self, SEL, Class c) { return objc::is_subclass(self->isa, c); }
BOOL NSObject_isMemberOfClass(id self, SEL, Class c) { return self->isa == c; }
BOOL NSObject_respondsToSelector(id self, SEL, SEL s) { return s && objc::responds_to(self->isa, s); }
BOOL NSObject_conformsToProtocol(id, SEL, void*) { return NO; }
BOOL NSObject_isEqual(id self, SEL, id o) { return self == o; }
NSUInteger NSObject_hash(id self, SEL) { return (NSUInteger)self >> 4; }
BOOL NSObject_isProxy(id, SEL) { return NO; }
void* NSObject_zone(id, SEL) { return nullptr; }
id NSObject_description(id self, SEL) {
    char buf[128];
    snprintf(buf, sizeof buf, "<%s: %p>", objc::class_name(self->isa), self);
    return ns::str_auto(std::string(buf));
}
id NSObject_copy(id self, SEL) { return objc::send(self, SEL_("copyWithZone:"), (void*)nullptr); }
id NSObject_mutableCopy(id self, SEL) { return objc::send(self, SEL_("mutableCopyWithZone:"), (void*)nullptr); }
id NSObject_performSelector(id self, SEL, SEL s) { return objc::send(self, s); }
id NSObject_performSelector1(id self, SEL, SEL s, id a) { return objc::send(self, s, a); }
id NSObject_performSelector2(id self, SEL, SEL s, id a, id b) { return objc::send(self, s, a, b); }
id NSObject_valueForKey(id self, SEL, id key) {
    std::string k = ns::to_utf8(key);
    SEL s = objc::sel(k.c_str());
    if (objc::responds_to(self->isa, s)) return objc::send(self, s);
    LOGW("valueForKey:%s on %s unsupported", k.c_str(), objc::class_name(self->isa));
    return nil;
}

// ---------------------------------------------------------------------------
// NSArray
// ---------------------------------------------------------------------------
using Vec = std::vector<id>;

id new_array(Class cls) {
    id o = objc::alloc_instance(cls);
    box<Vec>(o) = new Vec();
    return o;
}
void Array_dealloc(id self, SEL) {
    if (auto* v = box<Vec>(self)) {
        for (id x : *v) objc::release(x);
        delete v;
    }
    objc::dispose_instance(self);
}
id Array_alloc(Class cls, SEL) { return new_array(cls); }
id Array_init(id self, SEL) { return self; }
NSUInteger Array_count(id self, SEL) { return box<Vec>(self)->size(); }
id Array_objectAtIndex(id self, SEL, NSUInteger i) {
    auto* v = box<Vec>(self);
    if (i >= v->size()) {
        LOGE("NSArray index %lu beyond bounds %zu", i, v->size());
        return nil;
    }
    return (*v)[i];
}
id Array_lastObject(id self, SEL) {
    auto* v = box<Vec>(self);
    return v->empty() ? nil : v->back();
}
id Array_firstObject(id self, SEL) {
    auto* v = box<Vec>(self);
    return v->empty() ? nil : v->front();
}
BOOL Array_containsObject(id self, SEL, id o) {
    for (id x : *box<Vec>(self))
        if (x == o || objc::send<BOOL>(x, SEL_("isEqual:"), o)) return YES;
    return NO;
}
NSUInteger Array_indexOfObject(id self, SEL, id o) {
    auto* v = box<Vec>(self);
    for (size_t i = 0; i < v->size(); i++)
        if ((*v)[i] == o || objc::send<BOOL>((*v)[i], SEL_("isEqual:"), o)) return i;
    return (NSUInteger)LONG_MAX;
}
struct FastEnumState {
    unsigned long state;
    id* itemsPtr;
    unsigned long* mutationsPtr;
    unsigned long extra[5];
};
unsigned long g_mutations = 0;
NSUInteger Array_countByEnumerating(id self, SEL, FastEnumState* st, id*, NSUInteger) {
    if (st->state != 0) return 0;
    auto* v = box<Vec>(self);
    st->state = 1;
    st->itemsPtr = v->data();
    st->mutationsPtr = &g_mutations;
    return v->size();
}
id Array_copy(id self, SEL) {
    id o = new_array(g_NSArray);
    for (id x : *box<Vec>(self)) array_add(o, x);
    return o;
}
id Array_mutableCopy(id self, SEL) {
    id o = new_array(g_NSMutableArray);
    for (id x : *box<Vec>(self)) array_add(o, x);
    return o;
}
id Array_objectEnumerator(id self, SEL);
void Array_addObject(id self, SEL, id o) { array_add(self, o); }
void Array_removeObjectAtIndex(id self, SEL, NSUInteger i) {
    auto* v = box<Vec>(self);
    if (i < v->size()) {
        id o = (*v)[i];
        v->erase(v->begin() + i);
        objc::release(o);
    }
}
void Array_removeObject(id self, SEL, id o) {
    auto* v = box<Vec>(self);
    for (size_t i = 0; i < v->size();) {
        if ((*v)[i] == o || objc::send<BOOL>((*v)[i], SEL_("isEqual:"), o)) {
            id x = (*v)[i];
            v->erase(v->begin() + i);
            objc::release(x);
        } else i++;
    }
}
void Array_removeAllObjects(id self, SEL) {
    auto* v = box<Vec>(self);
    for (id x : *v) objc::release(x);
    v->clear();
}
void Array_removeLastObject(id self, SEL) {
    auto* v = box<Vec>(self);
    if (!v->empty()) {
        id x = v->back();
        v->pop_back();
        objc::release(x);
    }
}
void Array_insertObjectAtIndex(id self, SEL, id o, NSUInteger i) {
    auto* v = box<Vec>(self);
    if (i > v->size()) i = v->size();
    v->insert(v->begin() + i, objc::retain(o));
}
id Array_cls_array(Class cls, SEL) { return objc::autorelease(new_array(cls)); }
id Array_cls_arrayWithObject(Class cls, SEL, id o) {
    id a = new_array(cls);
    array_add(a, o);
    return objc::autorelease(a);
}
id Array_cls_arrayWithArray(Class cls, SEL, id other) {
    id a = new_array(cls);
    if (other)
        for (id x : *box<Vec>(other)) array_add(a, x);
    return objc::autorelease(a);
}
id Array_cls_arrayWithCapacity(Class cls, SEL, NSUInteger) { return objc::autorelease(new_array(cls)); }
id Array_description(id self, SEL) {
    std::string s = "(";
    for (id x : *box<Vec>(self)) {
        id d = objc::send(x, SEL_("description"));
        s += "\n    " + ns::to_utf8(d) + ",";
    }
    s += "\n)";
    return ns::str_auto(s);
}

// NSEnumerator over a snapshot vector.
struct EnumData {
    Vec items;
    size_t idx = 0;
};
id make_enumerator(const Vec& items) {
    id e = objc::alloc_instance(g_NSEnumerator);
    auto* d = new EnumData();
    d->items = items;
    for (id x : d->items) objc::retain(x);
    box<EnumData>(e) = d;
    return objc::autorelease(e);
}
id Enumerator_nextObject(id self, SEL) {
    auto* d = box<EnumData>(self);
    return d->idx < d->items.size() ? d->items[d->idx++] : nil;
}
id Enumerator_allObjects(id self, SEL) {
    auto* d = box<EnumData>(self);
    Vec rest(d->items.begin() + d->idx, d->items.end());
    d->idx = d->items.size();
    return objc::autorelease(make_array(rest));
}
void Enumerator_dealloc(id self, SEL) {
    if (auto* d = box<EnumData>(self)) {
        for (id x : d->items) objc::release(x);
        delete d;
    }
    objc::dispose_instance(self);
}
id Array_objectEnumerator(id self, SEL) { return make_enumerator(*box<Vec>(self)); }

// ---------------------------------------------------------------------------
// NSDictionary
// ---------------------------------------------------------------------------
using KV = std::vector<std::pair<id, id>>;

id new_dict(Class cls) {
    id o = objc::alloc_instance(cls);
    box<KV>(o) = new KV();
    return o;
}
bool keys_equal(id a, id b) {
    if (a == b) return true;
    if (!a || !b) return false;
    return objc::send<BOOL>(a, SEL_("isEqual:"), b);
}
void Dict_dealloc(id self, SEL) {
    if (auto* kv = box<KV>(self)) {
        for (auto& p : *kv) {
            objc::release(p.first);
            objc::release(p.second);
        }
        delete kv;
    }
    objc::dispose_instance(self);
}
id Dict_alloc(Class cls, SEL) { return new_dict(cls); }
id Dict_init(id self, SEL) { return self; }
NSUInteger Dict_count(id self, SEL) { return box<KV>(self)->size(); }
id Dict_objectForKey(id self, SEL, id key) { return dict_get(self, key); }
void Dict_setObjectForKey(id self, SEL, id obj, id key) { dict_set(self, key, obj); }
void Dict_removeObjectForKey(id self, SEL, id key) {
    auto* kv = box<KV>(self);
    for (size_t i = 0; i < kv->size(); i++)
        if (keys_equal((*kv)[i].first, key)) {
            objc::release((*kv)[i].first);
            objc::release((*kv)[i].second);
            kv->erase(kv->begin() + i);
            return;
        }
}
void Dict_removeAllObjects(id self, SEL) {
    auto* kv = box<KV>(self);
    for (auto& p : *kv) {
        objc::release(p.first);
        objc::release(p.second);
    }
    kv->clear();
}
id Dict_allKeys(id self, SEL) {
    Vec v;
    for (auto& p : *box<KV>(self)) v.push_back(p.first);
    return objc::autorelease(make_array(v));
}
id Dict_allValues(id self, SEL) {
    Vec v;
    for (auto& p : *box<KV>(self)) v.push_back(p.second);
    return objc::autorelease(make_array(v));
}
id Dict_keyEnumerator(id self, SEL) {
    Vec v;
    for (auto& p : *box<KV>(self)) v.push_back(p.first);
    return make_enumerator(v);
}
id Dict_objectEnumerator(id self, SEL) {
    Vec v;
    for (auto& p : *box<KV>(self)) v.push_back(p.second);
    return make_enumerator(v);
}
struct DictEnumBuf {
    Vec keys;
};
NSUInteger Dict_countByEnumerating(id self, SEL, FastEnumState* st, id* buf, NSUInteger len) {
    auto* kv = box<KV>(self);
    size_t start = st->state;
    if (start >= kv->size()) return 0;
    size_t n = 0;
    for (size_t i = start; i < kv->size() && n < len; i++, n++) buf[n] = (*kv)[i].first;
    st->state = start + n;
    st->itemsPtr = buf;
    st->mutationsPtr = &g_mutations;
    return n;
}
id Dict_copy(id self, SEL) {
    id o = new_dict(g_NSDictionary);
    for (auto& p : *box<KV>(self)) dict_set(o, p.first, p.second);
    return o;
}
id Dict_mutableCopy(id self, SEL) {
    id o = new_dict(g_NSMutableDictionary);
    for (auto& p : *box<KV>(self)) dict_set(o, p.first, p.second);
    return o;
}
id Dict_cls_dictionary(Class cls, SEL) { return objc::autorelease(new_dict(cls)); }
id Dict_cls_dictionaryWithObject_forKey(Class cls, SEL, id o, id k) {
    id d = new_dict(cls);
    dict_set(d, k, o);
    return objc::autorelease(d);
}
id Dict_cls_dictionaryWithDictionary(Class cls, SEL, id other) {
    id d = new_dict(cls);
    if (other)
        for (auto& p : *box<KV>(other)) dict_set(d, p.first, p.second);
    return objc::autorelease(d);
}
id Dict_description(id self, SEL) {
    std::string s = "{";
    for (auto& p : *box<KV>(self)) {
        s += "\n    " + ns::to_utf8(objc::send(p.first, SEL_("description"))) + " = " +
             ns::to_utf8(objc::send(p.second, SEL_("description"))) + ";";
    }
    s += "\n}";
    return ns::str_auto(s);
}

// ---------------------------------------------------------------------------
// NSSet
// ---------------------------------------------------------------------------
id Set_anyObject(id self, SEL) {
    auto* v = box<Vec>(self);
    return v->empty() ? nil : v->front();
}
id Set_allObjects(id self, SEL) { return objc::autorelease(make_array(*box<Vec>(self))); }
id Set_cls_setWithObject(Class cls, SEL, id o) {
    id s = new_array(cls);
    array_add(s, o);
    return objc::autorelease(s);
}
id Set_cls_set(Class cls, SEL) { return objc::autorelease(new_array(cls)); }
void Set_addObject(id self, SEL, id o) {
    for (id x : *box<Vec>(self))
        if (keys_equal(x, o)) return;
    array_add(self, o);
}

// ---------------------------------------------------------------------------
// NSNumber
// ---------------------------------------------------------------------------
struct NumObj {
    Class isa;
    int kind;  // 0 = int64, 1 = double, 2 = bool
    union {
        long long i;
        double d;
    };
};
id new_number(int kind, long long i, double d) {
    id o = objc::alloc_instance(g_NSNumber);
    auto* n = (NumObj*)o;
    n->kind = kind;
    if (kind == 1) n->d = d;
    else n->i = i;
    return o;
}
long long num_i(id self) {
    auto* n = (NumObj*)self;
    return n->kind == 1 ? (long long)n->d : n->i;
}
double num_d(id self) {
    auto* n = (NumObj*)self;
    return n->kind == 1 ? n->d : (double)n->i;
}
id Num_numberWithBool(Class, SEL, BOOL b) { return objc::autorelease(new_number(2, b ? 1 : 0, 0)); }
id Num_numberWithInt(Class, SEL, int v) { return objc::autorelease(new_number(0, v, 0)); }
id Num_numberWithInteger(Class, SEL, long v) { return objc::autorelease(new_number(0, v, 0)); }
id Num_numberWithUnsignedInt(Class, SEL, unsigned v) { return objc::autorelease(new_number(0, v, 0)); }
id Num_numberWithLongLong(Class, SEL, long long v) { return objc::autorelease(new_number(0, v, 0)); }
id Num_numberWithFloat(Class, SEL, float v) { return objc::autorelease(new_number(1, 0, v)); }
id Num_numberWithDouble(Class, SEL, double v) { return objc::autorelease(new_number(1, 0, v)); }
BOOL Num_boolValue(id self, SEL) { return num_i(self) != 0 || num_d(self) != 0.0; }
int Num_intValue(id self, SEL) { return (int)num_i(self); }
long Num_integerValue(id self, SEL) { return (long)num_i(self); }
long long Num_longLongValue(id self, SEL) { return num_i(self); }
unsigned long long Num_ullValue(id self, SEL) { return (unsigned long long)num_i(self); }
float Num_floatValue(id self, SEL) { return (float)num_d(self); }
double Num_doubleValue(id self, SEL) { return num_d(self); }
BOOL Num_isEqual(id self, SEL, id o) {
    if (!o || !objc::is_kind_of(o, g_NSNumber)) return NO;
    return num_d(self) == num_d(o);
}
NSUInteger Num_hash(id self, SEL) { return (NSUInteger)num_i(self); }
id Num_description(id self, SEL) {
    auto* n = (NumObj*)self;
    char buf[64];
    if (n->kind == 1) snprintf(buf, sizeof buf, "%g", n->d);
    else snprintf(buf, sizeof buf, "%lld", n->i);
    return ns::str_auto(std::string(buf));
}
id Num_stringValue(id self, SEL s) { return Num_description(self, s); }
id Num_copy(id self, SEL) { return objc::retain(self); }

// ---------------------------------------------------------------------------
// NSData
// ---------------------------------------------------------------------------
using Bytes = std::vector<uint8_t>;
id Data_alloc(Class cls, SEL) {
    id o = objc::alloc_instance(cls);
    box<Bytes>(o) = new Bytes();
    return o;
}
void Data_dealloc(id self, SEL) {
    delete box<Bytes>(self);
    objc::dispose_instance(self);
}
const void* Data_bytes(id self, SEL) { return box<Bytes>(self)->data(); }
NSUInteger Data_length(id self, SEL) { return box<Bytes>(self)->size(); }
id Data_cls_dataWithBytes(Class, SEL, const void* b, NSUInteger n) {
    return objc::autorelease(make_data(b, n));
}
id Data_cls_dataWithContentsOfFile(Class, SEL, id path) {
    FILE* f = fopen(ns::to_utf8(path).c_str(), "rb");
    if (!f) return nil;
    Bytes b;
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) b.insert(b.end(), buf, buf + n);
    fclose(f);
    return objc::autorelease(make_data(b.data(), b.size()));
}
BOOL Data_writeToFile(id self, SEL, id path, BOOL) {
    FILE* f = fopen(ns::to_utf8(path).c_str(), "wb");
    if (!f) return NO;
    auto* b = box<Bytes>(self);
    fwrite(b->data(), 1, b->size(), f);
    fclose(f);
    return YES;
}

// ---------------------------------------------------------------------------
// NSCharacterSet
// ---------------------------------------------------------------------------
struct CharsetObj {
    Class isa;
    int kind;  // 0 whitespace, 1 whitespace+newline
};
id g_ws = nil, g_wsnl = nil;
id Charset_whitespace(Class, SEL) { return g_ws; }
id Charset_whitespaceNewline(Class, SEL) { return g_wsnl; }
BOOL Charset_characterIsMember(id self, SEL, char16_t c) { return charset_contains(self, c); }

// ---------------------------------------------------------------------------
// NSURL / NSURLRequest
// ---------------------------------------------------------------------------
struct URLObj {
    Class isa;
    id string;  // full URL string ("file:///..." for file URLs)
    bool is_file;
};
id new_url(const std::string& s, bool file) {
    id o = objc::alloc_instance(g_NSURL);
    auto* u = (URLObj*)o;
    u->string = ns::make_string_utf8(s);
    u->is_file = file;
    return o;
}
void URL_dealloc(id self, SEL) {
    objc::release(((URLObj*)self)->string);
    objc::dispose_instance(self);
}
id URL_cls_fileURLWithPath(Class, SEL, id path) {
    return objc::autorelease(new_url("file://" + ns::to_utf8(path), true));
}
id URL_cls_fileURLWithPath_isDir(Class c, SEL s, id path, BOOL) { return URL_cls_fileURLWithPath(c, s, path); }
id URL_cls_URLWithString(Class, SEL, id str) {
    if (!str) return nil;
    std::string s = ns::to_utf8(str);
    return objc::autorelease(new_url(s, s.rfind("file://", 0) == 0));
}
id URL_initWithString(id self, SEL, id str) {
    std::string s = ns::to_utf8(str);
    auto* u = (URLObj*)self;
    u->string = ns::make_string_utf8(s);
    u->is_file = s.rfind("file://", 0) == 0;
    return self;
}
id URL_absoluteString(id self, SEL) { return ((URLObj*)self)->string; }
id URL_path(id self, SEL) { return ns::str_auto(url_path(self)); }
id URL_query(id self, SEL) {
    std::string s = url_string(self);
    size_t q = s.find('?');
    if (q == std::string::npos) return nil;
    size_t h = s.find('#', q);
    return ns::str_auto(s.substr(q + 1, h == std::string::npos ? std::string::npos : h - q - 1));
}
id URL_scheme(id self, SEL) {
    std::string s = url_string(self);
    size_t c = s.find(':');
    return c == std::string::npos ? nil : ns::str_auto(s.substr(0, c));
}
id URL_host(id self, SEL) {
    std::string s = url_string(self);
    size_t a = s.find("://");
    if (a == std::string::npos) return nil;
    a += 3;
    size_t e = s.find_first_of("/?#", a);
    return ns::str_auto(s.substr(a, e == std::string::npos ? std::string::npos : e - a));
}
BOOL URL_isFileURL(id self, SEL) { return ((URLObj*)self)->is_file; }
id URL_description(id self, SEL) { return ((URLObj*)self)->string; }

struct RequestObj {
    Class isa;
    id url;
};
id Request_cls_requestWithURL(Class, SEL, id url) {
    id o = objc::alloc_instance(g_NSURLRequest);
    ((RequestObj*)o)->url = objc::retain(url);
    return objc::autorelease(o);
}
id Request_initWithURL(id self, SEL, id url) {
    ((RequestObj*)self)->url = objc::retain(url);
    return self;
}
id Request_URL(id self, SEL) { return ((RequestObj*)self)->url; }

// ---------------------------------------------------------------------------
// NSError / NSNull / NSUUID
// ---------------------------------------------------------------------------
struct ErrorObj {
    Class isa;
    id domain;
    long code;
};
id Error_domain(id self, SEL) { return ((ErrorObj*)self)->domain; }
long Error_code(id self, SEL) { return ((ErrorObj*)self)->code; }
id Error_localizedDescription(id self, SEL) { return ns::str_auto(std::string("error")); }

id g_null = nil;
id Null_null(Class, SEL) { return g_null; }

id UUID_UUIDString(id self, SEL) {
    // Stable per-install identifier persisted in Documents.
    static std::string uuid;
    if (uuid.empty()) {
        std::string path = std::string(hle::library_path()) + "/.uuid";
        if (FILE* f = fopen(path.c_str(), "r")) {
            char buf[64] = {};
            if (fgets(buf, sizeof buf, f)) uuid = buf;
            fclose(f);
        }
        if (uuid.size() < 36) {
            char buf[64];
            srand((unsigned)time(nullptr));
            snprintf(buf, sizeof buf, "%08X-%04X-4%03X-A%03X-%04X%08X", rand(), rand() & 0xffff,
                     rand() & 0xfff, rand() & 0xfff, rand() & 0xffff, rand());
            uuid = buf;
            if (FILE* f = fopen(path.c_str(), "w")) {
                fputs(uuid.c_str(), f);
                fclose(f);
            }
        }
    }
    return ns::str_auto(uuid.substr(0, 36));
}

// ---------------------------------------------------------------------------
// Property list parsing
// ---------------------------------------------------------------------------
struct BPlist {
    const std::vector<uint8_t>& d;
    int offsz = 0, refsz = 0;
    uint64_t nobj = 0, top = 0, offtab = 0;

    uint64_t be(size_t off, int n) const {
        uint64_t v = 0;
        for (int i = 0; i < n; i++) v = (v << 8) | d.at(off + i);
        return v;
    }
    uint64_t obj_off(uint64_t idx) const { return be(offtab + idx * offsz, offsz); }
    uint64_t read_len(size_t& p, uint8_t low) const {
        if (low != 0xf) return low;
        uint8_t m = d.at(p++);
        int n = 1 << (m & 0xf);
        uint64_t v = be(p, n);
        p += n;
        return v;
    }
    id parse(uint64_t idx, int depth = 0) const {
        if (depth > 64) return nil;
        size_t p = obj_off(idx);
        uint8_t m = d.at(p++);
        uint8_t hi = m >> 4, lo = m & 0xf;
        switch (hi) {
            case 0x0:
                if (lo == 8 || lo == 9) return objc::retain(make_number_bool(lo == 9));
                return nil;
            case 0x1: {
                int n = 1 << lo;
                uint64_t v = be(p, n);
                long long sv = n == 8 ? (long long)v : n == 4 ? (long long)(int32_t)v : (long long)v;
                return objc::retain(make_number_int(sv));
            }
            case 0x2: {
                int n = 1 << lo;
                uint64_t v = be(p, n);
                double dv;
                if (n == 4) {
                    float f;
                    uint32_t w = (uint32_t)v;
                    memcpy(&f, &w, 4);
                    dv = f;
                } else memcpy(&dv, &v, 8);
                return objc::retain(make_number_double(dv));
            }
            case 0x4: {
                uint64_t n = read_len(p, lo);
                return make_data(d.data() + p, n);
            }
            case 0x5: {
                uint64_t n = read_len(p, lo);
                return ns::make_string_utf8((const char*)d.data() + p, n);
            }
            case 0x6: {
                uint64_t n = read_len(p, lo);
                std::u16string s;
                for (uint64_t i = 0; i < n; i++) s += (char16_t)be(p + i * 2, 2);
                return ns::make_string(s);
            }
            case 0xA: {
                uint64_t n = read_len(p, lo);
                id arr = make_mutable_array();
                for (uint64_t i = 0; i < n; i++) {
                    id v = parse(be(p + i * refsz, refsz), depth + 1);
                    array_add(arr, v);
                    objc::release(v);
                }
                return arr;
            }
            case 0xD: {
                uint64_t n = read_len(p, lo);
                id dict = make_mutable_dictionary();
                for (uint64_t i = 0; i < n; i++) {
                    id k = parse(be(p + i * refsz, refsz), depth + 1);
                    id v = parse(be(p + (n + i) * refsz, refsz), depth + 1);
                    dict_set(dict, k, v);
                    objc::release(k);
                    objc::release(v);
                }
                return dict;
            }
            default:
                return nil;
        }
    }
};

// Minimal XML plist reader.
struct XPlist {
    const std::string& s;
    size_t p = 0;
    void skip_ws() {
        for (;;) {
            while (p < s.size() && isspace((unsigned char)s[p])) p++;
            if (s.compare(p, 4, "<!--") == 0) {
                size_t e = s.find("-->", p);
                p = e == std::string::npos ? s.size() : e + 3;
            } else if (s.compare(p, 2, "<?") == 0 || s.compare(p, 2, "<!") == 0) {
                size_t e = s.find('>', p);
                p = e == std::string::npos ? s.size() : e + 1;
            } else break;
        }
    }
    std::string tag() {  // reads "<name...>" returning name (with leading '/' for close, trailing '/' for empty)
        skip_ws();
        if (p >= s.size() || s[p] != '<') return "";
        size_t e = s.find('>', p);
        std::string t = s.substr(p + 1, e - p - 1);
        p = e + 1;
        size_t sp = t.find(' ');
        if (sp != std::string::npos) {
            bool empty = t.back() == '/';
            t = t.substr(0, sp) + (empty ? "/" : "");
        }
        return t;
    }
    std::string text_until(const std::string& close) {
        size_t e = s.find("</" + close, p);
        std::string t = s.substr(p, e - p);
        p = s.find('>', e) + 1;
        std::string out;
        for (size_t i = 0; i < t.size(); i++) {
            if (t[i] == '&') {
                if (!t.compare(i, 5, "&amp;")) { out += '&'; i += 4; }
                else if (!t.compare(i, 4, "&lt;")) { out += '<'; i += 3; }
                else if (!t.compare(i, 4, "&gt;")) { out += '>'; i += 3; }
                else if (!t.compare(i, 6, "&quot;")) { out += '"'; i += 5; }
                else if (!t.compare(i, 6, "&apos;")) { out += '\''; i += 5; }
                else out += '&';
            } else out += t[i];
        }
        return out;
    }
    id value(const std::string& t) {
        if (t == "dict") {
            id d = make_mutable_dictionary();
            for (;;) {
                std::string k = tag();
                if (k == "/dict" || k.empty()) break;
                if (k == "key") {
                    std::string key = text_until("key");
                    std::string vt = tag();
                    id v = value(vt);
                    id ks = ns::make_string_utf8(key);
                    dict_set(d, ks, v);
                    objc::release(ks);
                    objc::release(v);
                }
            }
            return d;
        }
        if (t == "dict/") return make_mutable_dictionary();
        if (t == "array") {
            id a = make_mutable_array();
            for (;;) {
                std::string vt = tag();
                if (vt == "/array" || vt.empty()) break;
                id v = value(vt);
                array_add(a, v);
                objc::release(v);
            }
            return a;
        }
        if (t == "array/") return make_mutable_array();
        if (t == "string") return ns::make_string_utf8(text_until("string"));
        if (t == "string/") return ns::make_string_utf8("");
        if (t == "integer") return objc::retain(make_number_int(strtoll(text_until("integer").c_str(), nullptr, 10)));
        if (t == "real") return objc::retain(make_number_double(strtod(text_until("real").c_str(), nullptr)));
        if (t == "true/") return objc::retain(make_number_bool(true));
        if (t == "false/") return objc::retain(make_number_bool(false));
        if (t == "date") return ns::make_string_utf8(text_until("date"));
        if (t == "data") {
            std::string b64 = text_until("data"), raw;
            int val = 0, bits = -8;
            for (char c : b64) {
                const char* tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
                const char* f = strchr(tbl, c);
                if (!f || !c) continue;
                val = (val << 6) + (int)(f - tbl);
                bits += 6;
                if (bits >= 0) {
                    raw += (char)((val >> bits) & 0xff);
                    bits -= 8;
                }
            }
            return make_data(raw.data(), raw.size());
        }
        return nil;
    }
    id parse() {
        for (;;) {
            std::string t = tag();
            if (t.empty()) return nil;
            if (t == "plist") return value(tag());
        }
    }
};

}  // namespace

// --- public helpers ------------------------------------------------------------
id make_mutable_array() { return new_array(g_NSMutableArray); }
id make_array(const std::vector<id>& items) {
    id a = new_array(g_NSArray);
    for (id x : items) array_add(a, x);
    return a;
}
void array_add(id arr, id obj) {
    if (!obj) {
        LOGW("attempt to insert nil into array");
        return;
    }
    box<Vec>(arr)->push_back(objc::retain(obj));
}
std::vector<id>& array_items(id arr) { return *box<Vec>(arr); }

id make_mutable_dictionary() { return new_dict(g_NSMutableDictionary); }
void dict_set(id dict, id key, id value) {
    if (!key || !value) return;
    auto* kv = box<KV>(dict);
    for (auto& p : *kv)
        if (keys_equal(p.first, key)) {
            objc::retain(value);
            objc::release(p.second);
            p.second = value;
            return;
        }
    kv->push_back({objc::send(key, SEL_("copy")), objc::retain(value)});
}
id dict_get(id dict, id key) {
    if (!dict || !key) return nil;
    for (auto& p : *box<KV>(dict))
        if (keys_equal(p.first, key)) return p.second;
    return nil;
}
id dict_get(id dict, const char* key) {
    if (!dict) return nil;
    for (auto& p : *box<KV>(dict))
        if (ns::is_string(p.first) && ns::to_utf8(p.first) == key) return p.second;
    return nil;
}

id make_set(const std::vector<id>& items) {
    id s = new_array(g_NSSet);
    for (id x : items) array_add(s, x);
    return s;
}

id make_number_bool(bool v) { return objc::autorelease(new_number(2, v ? 1 : 0, 0)); }
id make_number_int(long long v) { return objc::autorelease(new_number(0, v, 0)); }
id make_number_double(double v) { return objc::autorelease(new_number(1, 0, v)); }

id make_data(const void* bytes, size_t n) {
    id o = objc::alloc_instance(g_NSData);
    box<Bytes>(o) = new Bytes((const uint8_t*)bytes, (const uint8_t*)bytes + n);
    return o;
}
const void* data_bytes(id d) { return box<Bytes>(d)->data(); }
size_t data_length(id d) { return box<Bytes>(d)->size(); }

bool charset_contains(id set, char16_t c) {
    int kind = set ? ((CharsetObj*)set)->kind : 0;
    if (c == ' ' || c == '\t' || c == 0xa0 || c == 0x3000) return true;
    if (kind == 1 && (c == '\n' || c == '\r' || c == 0x85 || c == 0x2028 || c == 0x2029)) return true;
    return false;
}

id make_file_url(const std::string& path) { return objc::autorelease(new_url("file://" + path, true)); }
std::string url_string(id url) { return url ? ns::to_utf8(((URLObj*)url)->string) : ""; }
std::string url_path(id url) {
    std::string s = url_string(url);
    if (s.rfind("file://", 0) == 0) s = s.substr(7);
    else {
        size_t a = s.find("://");
        if (a != std::string::npos) {
            size_t sl = s.find('/', a + 3);
            s = sl == std::string::npos ? "/" : s.substr(sl);
        }
    }
    size_t q = s.find_first_of("?#");
    if (q != std::string::npos) s = s.substr(0, q);
    // percent-decode
    std::string out;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '%' && i + 2 < s.size()) {
            out += (char)strtol(s.substr(i + 1, 2).c_str(), nullptr, 16);
            i += 2;
        } else out += s[i];
    }
    return out;
}

id parse_plist(const std::vector<uint8_t>& data) {
    if (data.size() >= 40 && !memcmp(data.data(), "bplist00", 8)) {
        BPlist bp{data};
        size_t t = data.size() - 32;
        bp.offsz = data[t + 6];
        bp.refsz = data[t + 7];
        bp.nobj = bp.be(t + 8, 8);
        bp.top = bp.be(t + 16, 8);
        bp.offtab = bp.be(t + 24, 8);
        return bp.parse(bp.top);
    }
    std::string s(data.begin(), data.end());
    XPlist xp{s};
    return xp.parse();
}

// --- CF helpers used by the game ----------------------------------------------
static long CFArrayGetCount(id a) { return a ? (long)box<Vec>(a)->size() : 0; }
static id CFArrayGetValueAtIndex(id a, long i) { return Array_objectAtIndex(a, nullptr, i); }
static id CFRetain(id o) { return objc::retain(o); }
static void CFRelease(id o) {
    if (!o) {
        LOGE("CFRelease(NULL)");
        return;
    }
    objc::release(o);
}
static void CFShow(id o) {
    if (!o) {
        LOGI("CFShow: (null)");
        return;
    }
    id d = ns::is_string(o) ? o : objc::send(o, SEL_("description"));
    LOGI("CFShow: %s", ns::to_utf8(d).c_str());
}
HLE_EXPORT(CFArrayGetCount, CFArrayGetCount);
HLE_EXPORT(CFArrayGetValueAtIndex, CFArrayGetValueAtIndex);
HLE_EXPORT(CFRetain, CFRetain);
HLE_EXPORT(CFRelease, CFRelease);
HLE_EXPORT(CFShow, CFShow);

static void* g_kCFAllocatorDefault = nullptr;
static void* g_kCFAllocatorNull = (void*)0x1;
HLE_EXPORT(kCFAllocatorDefault, &g_kCFAllocatorDefault);
HLE_EXPORT(kCFAllocatorNull, &g_kCFAllocatorNull);

static id URL_CFURLCreateCopyAppendingPathComponent(void*, id url, id comp, bool) {
    std::string p = url_path(url);
    if (!p.empty() && p.back() != '/') p += '/';
    p += ns::to_utf8(comp);
    return new_url("file://" + p, true);
}
static id URL_CFURLCreateCopyDeletingLastPathComponent(void*, id url) {
    std::string p = url_path(url);
    while (p.size() > 1 && p.back() == '/') p.pop_back();
    size_t s = p.rfind('/');
    p = s == std::string::npos ? "/" : p.substr(0, s + 1);
    return new_url("file://" + p, true);
}
static bool URL_CFURLGetFileSystemRepresentation(id url, bool, char* buf, long max) {
    std::string p = url_path(url);
    if ((long)p.size() + 1 > max) return false;
    memcpy(buf, p.c_str(), p.size() + 1);
    return true;
}
HLE_EXPORT(CFURLCreateCopyAppendingPathComponent, URL_CFURLCreateCopyAppendingPathComponent);
HLE_EXPORT(CFURLCreateCopyDeletingLastPathComponent, URL_CFURLCreateCopyDeletingLastPathComponent);
HLE_EXPORT(CFURLGetFileSystemRepresentation, URL_CFURLGetFileSystemRepresentation);

}  // namespace foundation

// Variadic +[NSDictionary dictionaryWithObjectsAndKeys:] (x3 = stack args).
extern "C" id hle_NSDictionary_dictionaryWithObjectsAndKeys_impl(Class cls, SEL, id first, const uintptr_t* va) {
    using namespace foundation;
    id d = new_dict(cls == g_NSDictionary ? g_NSDictionary : cls);
    id obj = first;
    while (obj) {
        id key = (id)*va++;
        if (!key) break;
        dict_set(d, key, obj);
        obj = (id)*va++;
    }
    return objc::autorelease(d);
}
extern "C" id hle_NSDictionary_dictionaryWithObjectsAndKeys(Class, SEL, id, ...);
extern "C" id hle_NSArray_arrayWithObjects_impl(Class cls, SEL, id first, const uintptr_t* va) {
    using namespace foundation;
    id a = new_array(cls);
    for (id o = first; o; o = (id)*va++) array_add(a, o);
    return objc::autorelease(a);
}
extern "C" id hle_NSArray_arrayWithObjects(Class, SEL, id, ...);

namespace foundation {

void init_sys();  // foundation_sys.cpp

void init() {
    using objc::add_class_method;
    using objc::add_method;
    g_NSObject = objc::define_class("NSObject", nullptr, sizeof(void*));
    Class O = g_NSObject;
    add_class_method(O, "alloc", (void*)NSObject_alloc);
    add_class_method(O, "allocWithZone:", (void*)NSObject_alloc);
    add_class_method(O, "new", (void*)NSObject_new);
    add_class_method(O, "class", (void*)NSObject_cls_class);
    add_class_method(O, "superclass", (void*)NSObject_cls_superclass);
    add_class_method(O, "description", (void*)NSObject_cls_description);
    add_class_method(O, "instancesRespondToSelector:", (void*)NSObject_cls_instancesRespond);
    add_class_method(O, "isSubclassOfClass:", (void*)NSObject_cls_isSubclassOfClass);
    add_class_method(O, "initialize", (void*)NSObject_cls_initialize);
    add_class_method(O, "load", (void*)NSObject_cls_initialize);
    add_method(O, "init", (void*)NSObject_init);
    add_method(O, "dealloc", (void*)NSObject_dealloc);
    add_method(O, "retain", (void*)NSObject_retain);
    add_method(O, "release", (void*)NSObject_release);
    add_method(O, "autorelease", (void*)NSObject_autorelease);
    add_method(O, "retainCount", (void*)NSObject_retainCount);
    add_method(O, "class", (void*)NSObject_class);
    add_method(O, "superclass", (void*)NSObject_superclass);
    add_method(O, "self", (void*)NSObject_self);
    add_method(O, "isKindOfClass:", (void*)NSObject_isKindOfClass);
    add_method(O, "isMemberOfClass:", (void*)NSObject_isMemberOfClass);
    add_method(O, "respondsToSelector:", (void*)NSObject_respondsToSelector);
    add_method(O, "conformsToProtocol:", (void*)NSObject_conformsToProtocol);
    add_method(O, "isEqual:", (void*)NSObject_isEqual);
    add_method(O, "hash", (void*)NSObject_hash);
    add_method(O, "isProxy", (void*)NSObject_isProxy);
    add_method(O, "zone", (void*)NSObject_zone);
    add_method(O, "description", (void*)NSObject_description);
    add_method(O, "debugDescription", (void*)NSObject_description);
    add_method(O, "copy", (void*)NSObject_copy);
    add_method(O, "mutableCopy", (void*)NSObject_mutableCopy);
    add_method(O, "performSelector:", (void*)NSObject_performSelector);
    add_method(O, "performSelector:withObject:", (void*)NSObject_performSelector1);
    add_method(O, "performSelector:withObject:withObject:", (void*)NSObject_performSelector2);
    add_method(O, "valueForKey:", (void*)NSObject_valueForKey);

    init_strings();

    g_NSArray = objc::define_class("NSArray", "NSObject", 16);
    g_NSMutableArray = objc::define_class("NSMutableArray", "NSArray", 16);
    Class A = g_NSArray;
    add_class_method(A, "alloc", (void*)Array_alloc);
    add_class_method(A, "array", (void*)Array_cls_array);
    add_class_method(A, "arrayWithObject:", (void*)Array_cls_arrayWithObject);
    add_class_method(A, "arrayWithArray:", (void*)Array_cls_arrayWithArray);
    add_class_method(A, "arrayWithObjects:", (void*)hle_NSArray_arrayWithObjects);
    add_class_method(g_NSMutableArray, "arrayWithCapacity:", (void*)Array_cls_arrayWithCapacity);
    add_method(A, "init", (void*)Array_init);
    add_method(A, "initWithCapacity:", (void*)Array_init);
    add_method(A, "dealloc", (void*)Array_dealloc);
    add_method(A, "count", (void*)Array_count);
    add_method(A, "objectAtIndex:", (void*)Array_objectAtIndex);
    add_method(A, "objectAtIndexedSubscript:", (void*)Array_objectAtIndex);
    add_method(A, "lastObject", (void*)Array_lastObject);
    add_method(A, "firstObject", (void*)Array_firstObject);
    add_method(A, "containsObject:", (void*)Array_containsObject);
    add_method(A, "indexOfObject:", (void*)Array_indexOfObject);
    add_method(A, "countByEnumeratingWithState:objects:count:", (void*)Array_countByEnumerating);
    add_method(A, "objectEnumerator", (void*)Array_objectEnumerator);
    add_method(A, "copy", (void*)Array_copy);
    add_method(A, "copyWithZone:", (void*)Array_copy);
    add_method(A, "mutableCopy", (void*)Array_mutableCopy);
    add_method(A, "description", (void*)Array_description);
    Class MA = g_NSMutableArray;
    add_method(MA, "addObject:", (void*)Array_addObject);
    add_method(MA, "removeObjectAtIndex:", (void*)Array_removeObjectAtIndex);
    add_method(MA, "removeObject:", (void*)Array_removeObject);
    add_method(MA, "removeAllObjects", (void*)Array_removeAllObjects);
    add_method(MA, "removeLastObject", (void*)Array_removeLastObject);
    add_method(MA, "insertObject:atIndex:", (void*)Array_insertObjectAtIndex);

    g_NSEnumerator = objc::define_class("NSEnumerator", "NSObject", 16);
    add_method(g_NSEnumerator, "nextObject", (void*)Enumerator_nextObject);
    add_method(g_NSEnumerator, "allObjects", (void*)Enumerator_allObjects);
    add_method(g_NSEnumerator, "dealloc", (void*)Enumerator_dealloc);

    g_NSDictionary = objc::define_class("NSDictionary", "NSObject", 16);
    g_NSMutableDictionary = objc::define_class("NSMutableDictionary", "NSDictionary", 16);
    Class D = g_NSDictionary;
    add_class_method(D, "alloc", (void*)Dict_alloc);
    add_class_method(D, "dictionary", (void*)Dict_cls_dictionary);
    add_class_method(D, "dictionaryWithObject:forKey:", (void*)Dict_cls_dictionaryWithObject_forKey);
    add_class_method(D, "dictionaryWithDictionary:", (void*)Dict_cls_dictionaryWithDictionary);
    add_class_method(D, "dictionaryWithObjectsAndKeys:", (void*)hle_NSDictionary_dictionaryWithObjectsAndKeys);
    add_class_method(g_NSMutableDictionary, "dictionaryWithCapacity:", (void*)Dict_cls_dictionary);
    add_method(D, "init", (void*)Dict_init);
    add_method(D, "initWithCapacity:", (void*)Dict_init);
    add_method(D, "dealloc", (void*)Dict_dealloc);
    add_method(D, "count", (void*)Dict_count);
    add_method(D, "objectForKey:", (void*)Dict_objectForKey);
    add_method(D, "valueForKey:", (void*)Dict_objectForKey);
    add_method(D, "objectForKeyedSubscript:", (void*)Dict_objectForKey);
    add_method(D, "allKeys", (void*)Dict_allKeys);
    add_method(D, "allValues", (void*)Dict_allValues);
    add_method(D, "keyEnumerator", (void*)Dict_keyEnumerator);
    add_method(D, "objectEnumerator", (void*)Dict_objectEnumerator);
    add_method(D, "countByEnumeratingWithState:objects:count:", (void*)Dict_countByEnumerating);
    add_method(D, "copy", (void*)Dict_copy);
    add_method(D, "copyWithZone:", (void*)Dict_copy);
    add_method(D, "mutableCopy", (void*)Dict_mutableCopy);
    add_method(D, "description", (void*)Dict_description);
    Class MD = g_NSMutableDictionary;
    add_method(MD, "setObject:forKey:", (void*)Dict_setObjectForKey);
    add_method(MD, "setValue:forKey:", (void*)Dict_setObjectForKey);
    add_method(MD, "removeObjectForKey:", (void*)Dict_removeObjectForKey);
    add_method(MD, "removeAllObjects", (void*)Dict_removeAllObjects);

    // NSSet shares the vector-backed layout with NSArray.
    g_NSSet = objc::define_class("NSSet", "NSObject", 16);
    g_NSMutableSet = objc::define_class("NSMutableSet", "NSSet", 16);
    Class St = g_NSSet;
    add_class_method(St, "alloc", (void*)Array_alloc);
    add_class_method(St, "set", (void*)Set_cls_set);
    add_class_method(St, "setWithObject:", (void*)Set_cls_setWithObject);
    add_method(St, "init", (void*)Array_init);
    add_method(St, "dealloc", (void*)Array_dealloc);
    add_method(St, "count", (void*)Array_count);
    add_method(St, "anyObject", (void*)Set_anyObject);
    add_method(St, "allObjects", (void*)Set_allObjects);
    add_method(St, "containsObject:", (void*)Array_containsObject);
    add_method(St, "member:", (void*)Array_containsObject);
    add_method(St, "objectEnumerator", (void*)Array_objectEnumerator);
    add_method(St, "countByEnumeratingWithState:objects:count:", (void*)Array_countByEnumerating);
    add_method(g_NSMutableSet, "addObject:", (void*)Set_addObject);
    add_method(g_NSMutableSet, "removeObject:", (void*)Array_removeObject);
    add_method(g_NSMutableSet, "removeAllObjects", (void*)Array_removeAllObjects);

    g_NSValue = objc::define_class("NSValue", "NSObject", sizeof(NumObj));
    g_NSNumber = objc::define_class("NSNumber", "NSValue", sizeof(NumObj));
    Class N = g_NSNumber;
    add_class_method(N, "numberWithBool:", (void*)Num_numberWithBool);
    add_class_method(N, "numberWithInt:", (void*)Num_numberWithInt);
    add_class_method(N, "numberWithInteger:", (void*)Num_numberWithInteger);
    add_class_method(N, "numberWithUnsignedInt:", (void*)Num_numberWithUnsignedInt);
    add_class_method(N, "numberWithLongLong:", (void*)Num_numberWithLongLong);
    add_class_method(N, "numberWithFloat:", (void*)Num_numberWithFloat);
    add_class_method(N, "numberWithDouble:", (void*)Num_numberWithDouble);
    add_method(N, "boolValue", (void*)Num_boolValue);
    add_method(N, "intValue", (void*)Num_intValue);
    add_method(N, "integerValue", (void*)Num_integerValue);
    add_method(N, "longLongValue", (void*)Num_longLongValue);
    add_method(N, "unsignedLongLongValue", (void*)Num_ullValue);
    add_method(N, "unsignedIntValue", (void*)Num_intValue);
    add_method(N, "floatValue", (void*)Num_floatValue);
    add_method(N, "doubleValue", (void*)Num_doubleValue);
    add_method(N, "isEqual:", (void*)Num_isEqual);
    add_method(N, "isEqualToNumber:", (void*)Num_isEqual);
    add_method(N, "hash", (void*)Num_hash);
    add_method(N, "description", (void*)Num_description);
    add_method(N, "stringValue", (void*)Num_stringValue);
    add_method(N, "copy", (void*)Num_copy);
    add_method(N, "copyWithZone:", (void*)Num_copy);

    g_NSData = objc::define_class("NSData", "NSObject", 16);
    g_NSMutableData = objc::define_class("NSMutableData", "NSData", 16);
    add_class_method(g_NSData, "alloc", (void*)Data_alloc);
    add_class_method(g_NSData, "dataWithBytes:length:", (void*)Data_cls_dataWithBytes);
    add_class_method(g_NSData, "dataWithContentsOfFile:", (void*)Data_cls_dataWithContentsOfFile);
    add_method(g_NSData, "dealloc", (void*)Data_dealloc);
    add_method(g_NSData, "bytes", (void*)Data_bytes);
    add_method(g_NSData, "length", (void*)Data_length);
    add_method(g_NSData, "writeToFile:atomically:", (void*)Data_writeToFile);

    g_NSCharacterSet = objc::define_class("NSCharacterSet", "NSObject", sizeof(CharsetObj));
    add_class_method(g_NSCharacterSet, "whitespaceCharacterSet", (void*)Charset_whitespace);
    add_class_method(g_NSCharacterSet, "whitespaceAndNewlineCharacterSet", (void*)Charset_whitespaceNewline);
    add_method(g_NSCharacterSet, "characterIsMember:", (void*)Charset_characterIsMember);
    g_ws = objc::alloc_instance(g_NSCharacterSet);
    ((CharsetObj*)g_ws)->kind = 0;
    g_wsnl = objc::alloc_instance(g_NSCharacterSet);
    ((CharsetObj*)g_wsnl)->kind = 1;

    g_NSURL = objc::define_class("NSURL", "NSObject", sizeof(URLObj));
    Class U = g_NSURL;
    add_class_method(U, "fileURLWithPath:", (void*)URL_cls_fileURLWithPath);
    add_class_method(U, "fileURLWithPath:isDirectory:", (void*)URL_cls_fileURLWithPath_isDir);
    add_class_method(U, "URLWithString:", (void*)URL_cls_URLWithString);
    add_method(U, "initWithString:", (void*)URL_initWithString);
    add_method(U, "dealloc", (void*)URL_dealloc);
    add_method(U, "absoluteString", (void*)URL_absoluteString);
    add_method(U, "path", (void*)URL_path);
    add_method(U, "query", (void*)URL_query);
    add_method(U, "scheme", (void*)URL_scheme);
    add_method(U, "host", (void*)URL_host);
    add_method(U, "isFileURL", (void*)URL_isFileURL);
    add_method(U, "description", (void*)URL_description);

    g_NSURLRequest = objc::define_class("NSURLRequest", "NSObject", sizeof(RequestObj));
    add_class_method(g_NSURLRequest, "requestWithURL:", (void*)Request_cls_requestWithURL);
    add_method(g_NSURLRequest, "initWithURL:", (void*)Request_initWithURL);
    add_method(g_NSURLRequest, "URL", (void*)Request_URL);

    g_NSError = objc::define_class("NSError", "NSObject", sizeof(ErrorObj));
    add_method(g_NSError, "domain", (void*)Error_domain);
    add_method(g_NSError, "code", (void*)Error_code);
    add_method(g_NSError, "localizedDescription", (void*)Error_localizedDescription);

    g_NSNull = objc::define_class("NSNull", "NSObject", 8);
    g_null = objc::alloc_instance(g_NSNull);
    add_class_method(g_NSNull, "null", (void*)Null_null);

    g_NSUUID = objc::define_class("NSUUID", "NSObject", 8);
    add_method(g_NSUUID, "UUIDString", (void*)UUID_UUIDString);

    // Blocks: global block literals in the image use this as their isa.
    Class GB = objc::define_class("__NSGlobalBlock__", "NSObject", 32);
    add_method(GB, "copy", (void*)NSObject_self);
    add_method(GB, "copyWithZone:", (void*)NSObject_self);
    add_method(GB, "retain", (void*)NSObject_self);
    add_method(GB, "autorelease", (void*)NSObject_self);
    add_method(GB, "release", (void*)+[](id, SEL) {});
    hle::register_symbol("__NSConcreteGlobalBlock", GB);

    init_sys();
}

}  // namespace foundation
