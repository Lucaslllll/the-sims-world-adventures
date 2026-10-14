// Foundation system services: NSBundle, NSFileManager, locks, autorelease
// pools, notifications, timers/run loop, locale, NSLog.
#include <dirent.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <future>
#include <mutex>
#include <vector>

#include "hle/config.h"
#include "hle/format.h"
#include "hle/foundation.h"
#include "hle/nsstring.h"
#include "objc/runtime.h"

namespace hle {

Config& config() {
    static Config c;
    return c;
}

static std::string g_documents, g_library, g_caches;
const char* bundle_path() { return config().bundle_path.c_str(); }
const char* documents_path() { return g_documents.c_str(); }
const char* library_path() { return g_library.c_str(); }
const char* caches_path() { return g_caches.c_str(); }

}  // namespace hle

namespace foundation {

using hle::config;

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

Class g_NSBundle, g_NSFileManager, g_NSLock, g_NSRecursiveLock, g_NSAutoreleasePool,
    g_NSNotificationCenter, g_NSNotification, g_NSTimer, g_NSRunLoop, g_NSLocale;

pthread_t g_main_thread;

std::string read_file(const std::string& path, bool* ok = nullptr) {
    std::string out;
    FILE* f = fopen(path.c_str(), "rb");
    if (ok) *ok = f != nullptr;
    if (!f) return out;
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
    fclose(f);
    return out;
}

bool file_exists(const std::string& p, bool* is_dir = nullptr) {
    struct stat st;
    if (stat(p.c_str(), &st) != 0) return false;
    if (is_dir) *is_dir = S_ISDIR(st.st_mode);
    return true;
}

bool mkdirs(const std::string& path) {
    std::string cur;
    size_t i = 0;
    while (i <= path.size()) {
        size_t j = path.find('/', i);
        if (j == std::string::npos) j = path.size();
        cur = path.substr(0, j);
        if (!cur.empty()) mkdir(cur.c_str(), 0755);
        i = j + 1;
    }
    bool d = false;
    return file_exists(path, &d) && d;
}

bool remove_recursive(const std::string& path) {
    bool dir = false;
    if (!file_exists(path, &dir)) return false;
    if (dir) {
        if (DIR* d = opendir(path.c_str())) {
            while (dirent* e = readdir(d)) {
                if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
                remove_recursive(path + "/" + e->d_name);
            }
            closedir(d);
        }
        return rmdir(path.c_str()) == 0;
    }
    return unlink(path.c_str()) == 0;
}

bool copy_recursive(const std::string& from, const std::string& to) {
    bool dir = false;
    if (!file_exists(from, &dir)) return false;
    if (dir) {
        mkdirs(to);
        if (DIR* d = opendir(from.c_str())) {
            while (dirent* e = readdir(d)) {
                if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
                copy_recursive(from + "/" + e->d_name, to + "/" + e->d_name);
            }
            closedir(d);
        }
        return true;
    }
    bool ok;
    std::string data = read_file(from, &ok);
    if (!ok) return false;
    FILE* f = fopen(to.c_str(), "wb");
    if (!f) return false;
    fwrite(data.data(), 1, data.size(), f);
    fclose(f);
    return true;
}

std::vector<std::string> list_dir(const std::string& path) {
    std::vector<std::string> out;
    if (DIR* d = opendir(path.c_str())) {
        while (dirent* e = readdir(d)) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            out.push_back(e->d_name);
        }
        closedir(d);
    }
    return out;
}

void set_error(id* err, int code) {
    if (err) *err = nil;
    (void)code;
}

// ---------------------------------------------------------------------------
// NSBundle
// ---------------------------------------------------------------------------
struct BundleData {
    std::string path;
    id info = nil;
    std::vector<std::string> localizations;
};
id g_main_bundle = nil;

BundleData* bundle_data(id b) { return box<BundleData>(b); }

id main_bundle() {
    if (g_main_bundle) return g_main_bundle;
    g_main_bundle = objc::alloc_instance(g_NSBundle);
    auto* d = new BundleData();
    d->path = config().bundle_path;
    std::string plist = read_file(d->path + "/Info.plist");
    d->info = parse_plist(std::vector<uint8_t>(plist.begin(), plist.end()));
    if (!d->info) {
        LOGE("could not parse %s/Info.plist", d->path.c_str());
        d->info = make_mutable_dictionary();
    }
    for (auto& n : list_dir(d->path))
        if (n.size() > 6 && n.compare(n.size() - 6, 6, ".lproj") == 0) d->localizations.push_back(n.substr(0, n.size() - 6));
    box<BundleData>(g_main_bundle) = d;
    return g_main_bundle;
}

// Locate a resource like CFBundle does: bundle root (or subdir), then the
// preferred .lproj, then en.lproj / Base.lproj.
std::string find_resource(BundleData* b, std::string name, std::string type, const std::string& subdir) {
    if (!type.empty()) {
        if (type[0] == '.') type = type.substr(1);
        name += "." + type;
    }
    std::string base = b->path;
    if (!subdir.empty()) base += "/" + subdir;
    std::vector<std::string> dirs = {base};
    std::string lang = config().language;
    for (auto& l : b->localizations)
        if (l == lang || l.rfind(lang + "_", 0) == 0) dirs.push_back(base + "/" + l + ".lproj");
    dirs.push_back(base + "/en.lproj");
    dirs.push_back(base + "/Base.lproj");
    for (auto& d : dirs) {
        std::string p = d + "/" + name;
        if (file_exists(p)) return p;
    }
    return "";
}

id Bundle_mainBundle(Class, SEL) { return main_bundle(); }
id Bundle_bundlePath(id self, SEL) { return ns::str_auto(bundle_data(self)->path); }
id Bundle_bundleIdentifier(id self, SEL) { return dict_get(bundle_data(self)->info, "CFBundleIdentifier"); }
id Bundle_infoDictionary(id self, SEL) { return bundle_data(self)->info; }
id Bundle_objectForInfoDictionaryKey(id self, SEL, id key) {
    std::string k = ns::to_utf8(key);
    if (k == "EAGLRenderingAPI") return ns::str_auto(config().gl_api);
    id v = dict_get(bundle_data(self)->info, key);
    LOGD("Info.plist[%s] -> %s", k.c_str(), v ? ns::to_utf8(objc::send(v, SEL_("description"))).c_str() : "nil");
    return v;
}
id Bundle_pathForResource_ofType_inDirectory(id self, SEL, id name, id type, id dir) {
    if (!name) return nil;
    std::string p = find_resource(bundle_data(self), ns::to_utf8(name), type ? ns::to_utf8(type) : "",
                                  dir ? ns::to_utf8(dir) : "");
    TRACE("pathForResource %s.%s in %s -> %s", ns::to_utf8(name).c_str(), type ? ns::to_utf8(type).c_str() : "",
          dir ? ns::to_utf8(dir).c_str() : "", p.c_str());
    return p.empty() ? nil : ns::str_auto(p);
}
id Bundle_pathForResource_ofType(id self, SEL s, id name, id type) {
    return Bundle_pathForResource_ofType_inDirectory(self, s, name, type, nil);
}
id Bundle_URLForResource(id self, SEL s, id name, id ext) {
    id p = Bundle_pathForResource_ofType_inDirectory(self, s, name, ext, nil);
    return p ? make_file_url(ns::to_utf8(p)) : nil;
}
id Bundle_resourcePath(id self, SEL) { return ns::str_auto(bundle_data(self)->path); }
id Bundle_resourceURL(id self, SEL) { return make_file_url(bundle_data(self)->path); }
id Bundle_bundleURL(id self, SEL) { return make_file_url(bundle_data(self)->path); }
id Bundle_localizations(id self, SEL) {
    std::vector<id> v;
    for (auto& l : bundle_data(self)->localizations) v.push_back(ns::str_auto(l));
    return objc::autorelease(make_array(v));
}
id Bundle_preferredLocalizations(id self, SEL) {
    std::string lang = config().language;
    for (auto& l : bundle_data(self)->localizations)
        if (l == lang || l.rfind(lang + "_", 0) == 0) return objc::autorelease(make_array({ns::str_auto(l)}));
    return objc::autorelease(make_array({ns::str_auto(std::string("en"))}));
}
id Bundle_localizedString(id self, SEL, id key, id value, id table) { return value ? value : key; }

// ---------------------------------------------------------------------------
// NSFileManager
// ---------------------------------------------------------------------------
id g_file_manager = nil;
id FM_defaultManager(Class, SEL) {
    if (!g_file_manager) g_file_manager = objc::alloc_instance(g_NSFileManager);
    return g_file_manager;
}
BOOL FM_fileExistsAtPath(id, SEL, id path) { return path && file_exists(ns::to_utf8(path)); }
BOOL FM_fileExistsAtPath_isDirectory(id, SEL, id path, BOOL* isDir) {
    bool d = false;
    bool e = path && file_exists(ns::to_utf8(path), &d);
    if (isDir) *isDir = d;
    return e;
}
BOOL FM_isReadable(id, SEL, id p) { return p && access(ns::to_utf8(p).c_str(), R_OK) == 0; }
BOOL FM_isWritable(id, SEL, id p) { return p && access(ns::to_utf8(p).c_str(), W_OK) == 0; }
BOOL FM_isExecutable(id, SEL, id p) { return p && access(ns::to_utf8(p).c_str(), X_OK) == 0; }
BOOL FM_createDirectory(id, SEL, id path, BOOL intermediate, id, id* err) {
    std::string p = ns::to_utf8(path);
    bool ok = intermediate ? mkdirs(p) : mkdir(p.c_str(), 0755) == 0;
    set_error(err, ok ? 0 : 1);
    return ok;
}
BOOL FM_removeItem(id, SEL, id path, id* err) {
    bool ok = remove_recursive(ns::to_utf8(path));
    set_error(err, ok ? 0 : 1);
    return ok;
}
BOOL FM_copyItem(id, SEL, id from, id to, id* err) {
    std::string t = ns::to_utf8(to);
    if (file_exists(t)) {
        set_error(err, 1);
        return NO;
    }
    bool ok = copy_recursive(ns::to_utf8(from), t);
    set_error(err, ok ? 0 : 1);
    return ok;
}
BOOL FM_moveItem(id, SEL, id from, id to, id* err) {
    bool ok = rename(ns::to_utf8(from).c_str(), ns::to_utf8(to).c_str()) == 0;
    set_error(err, ok ? 0 : 1);
    return ok;
}
id FM_contentsOfDirectory(id, SEL, id path, id* err) {
    std::string p = ns::to_utf8(path);
    bool d = false;
    if (!file_exists(p, &d) || !d) {
        set_error(err, 1);
        return nil;
    }
    std::vector<id> v;
    for (auto& n : list_dir(p)) v.push_back(ns::str_auto(n));
    return objc::autorelease(make_array(v));
}
id FM_directoryContents(id self, SEL s, id path) { return FM_contentsOfDirectory(self, s, path, nullptr); }

id g_NSFileSystemSize = nil, g_NSFileSystemFreeSize = nil, g_NSFileSize = nil;
id FM_attributesOfFileSystem(id, SEL, id path, id* err) {
    struct statvfs st;
    std::string p = path ? ns::to_utf8(path) : config().home_path;
    if (statvfs(p.c_str(), &st) != 0) statvfs(config().home_path.c_str(), &st);
    id d = make_mutable_dictionary();
    dict_set(d, g_NSFileSystemSize, make_number_int((long long)st.f_blocks * st.f_frsize));
    dict_set(d, g_NSFileSystemFreeSize, make_number_int((long long)st.f_bavail * st.f_frsize));
    set_error(err, 0);
    return objc::autorelease(d);
}
id FM_attributesOfItem(id, SEL, id path, id* err) {
    struct stat st;
    if (!path || stat(ns::to_utf8(path).c_str(), &st) != 0) {
        set_error(err, 1);
        return nil;
    }
    id d = make_mutable_dictionary();
    dict_set(d, g_NSFileSize, make_number_int(st.st_size));
    return objc::autorelease(d);
}
BOOL FM_createFile(id, SEL, id path, id data, id) {
    FILE* f = fopen(ns::to_utf8(path).c_str(), "wb");
    if (!f) return NO;
    if (data) fwrite(data_bytes(data), 1, data_length(data), f);
    fclose(f);
    return YES;
}
id FM_contentsAtPath(id, SEL, id path) {
    bool ok;
    std::string s = read_file(ns::to_utf8(path), &ok);
    return ok ? objc::autorelease(make_data(s.data(), s.size())) : nil;
}

// ---------------------------------------------------------------------------
// Locks and autorelease pools
// ---------------------------------------------------------------------------
id Lock_alloc(Class cls, SEL) {
    id o = objc::alloc_instance(cls);
    auto* m = new pthread_mutex_t;
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    if (objc::is_subclass(cls, g_NSRecursiveLock)) pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(m, &a);
    pthread_mutexattr_destroy(&a);
    box<pthread_mutex_t>(o) = m;
    return o;
}
void Lock_dealloc(id self, SEL) {
    pthread_mutex_destroy(box<pthread_mutex_t>(self));
    delete box<pthread_mutex_t>(self);
    objc::dispose_instance(self);
}
void Lock_lock(id self, SEL) { pthread_mutex_lock(box<pthread_mutex_t>(self)); }
void Lock_unlock(id self, SEL) { pthread_mutex_unlock(box<pthread_mutex_t>(self)); }
BOOL Lock_tryLock(id self, SEL) { return pthread_mutex_trylock(box<pthread_mutex_t>(self)) == 0; }

id Pool_init(id self, SEL) {
    box<void>(self) = objc::pool_push();
    return self;
}
void Pool_drain(id self, SEL) {
    objc::pool_pop(box<void>(self));
    objc::dispose_instance(self);
}
id Pool_retain(id self, SEL) { return self; }

// ---------------------------------------------------------------------------
// Notifications
// ---------------------------------------------------------------------------
struct Observer {
    id observer;
    SEL sel;
    id name;
    id object;
};
std::mutex g_obs_mutex;
std::vector<Observer> g_observers;
id g_center = nil;

struct NotificationObj {
    Class isa;
    id name, object, user_info;
};

id NC_defaultCenter(Class, SEL) {
    if (!g_center) g_center = objc::alloc_instance(g_NSNotificationCenter);
    return g_center;
}
void NC_addObserver(id, SEL, id observer, SEL sel, id name, id object) {
    std::lock_guard<std::mutex> lk(g_obs_mutex);
    g_observers.push_back({observer, sel, objc::retain(name), object});
    TRACE("addObserver %s for %s", sel, name ? ns::to_utf8(name).c_str() : "*");
}
void NC_removeObserver_name_object(id, SEL, id observer, id name, id object) {
    std::lock_guard<std::mutex> lk(g_obs_mutex);
    for (size_t i = 0; i < g_observers.size();) {
        auto& o = g_observers[i];
        bool match = o.observer == observer &&
                     (!name || (o.name && ns::to_u16(o.name) == ns::to_u16(name))) &&
                     (!object || o.object == object);
        if (match) {
            objc::release(o.name);
            g_observers.erase(g_observers.begin() + i);
        } else i++;
    }
}
void NC_removeObserver(id self, SEL s, id observer) { NC_removeObserver_name_object(self, s, observer, nil, nil); }
void NC_postNotificationName_object_userInfo(id, SEL, id name, id object, id user_info) {
    post_notification(name, object, user_info);
}
void NC_postNotificationName_object(id self, SEL s, id name, id object) {
    post_notification(name, object, nil);
}
void NC_postNotification(id, SEL, id n) {
    auto* no = (NotificationObj*)n;
    post_notification(no->name, no->object, no->user_info);
}
id Notif_name(id self, SEL) { return ((NotificationObj*)self)->name; }
id Notif_object(id self, SEL) { return ((NotificationObj*)self)->object; }
id Notif_userInfo(id self, SEL) { return ((NotificationObj*)self)->user_info; }
void Notif_dealloc(id self, SEL) {
    auto* n = (NotificationObj*)self;
    objc::release(n->name);
    objc::release(n->object);
    objc::release(n->user_info);
    objc::dispose_instance(self);
}

// ---------------------------------------------------------------------------
// Timers and the run loop
// ---------------------------------------------------------------------------
struct TimerData {
    double interval = 0;
    id target = nil;
    SEL sel = nullptr;
    id user_info = nil;
    bool repeats = false;
    bool valid = true;
    bool scheduled = false;
    double fire_at = 0;
    std::function<void()> fn;
};
std::recursive_mutex g_rl_mutex;
std::vector<id> g_timers;
std::mutex g_queue_mutex;
std::deque<std::function<void()>> g_main_queue;
id g_main_runloop = nil;

TimerData* tdata(id t) { return box<TimerData>(t); }

id new_timer(double interval, id target, SEL sel, id user_info, bool repeats) {
    id t = objc::alloc_instance(g_NSTimer);
    auto* d = new TimerData();
    d->interval = interval < 0.0001 ? 0.0001 : interval;
    d->target = objc::retain(target);
    d->sel = sel;
    d->user_info = objc::retain(user_info);
    d->repeats = repeats;
    box<TimerData>(t) = d;
    return t;
}
void schedule_timer(id t) {
    std::lock_guard<std::recursive_mutex> lk(g_rl_mutex);
    auto* d = tdata(t);
    if (d->scheduled || !d->valid) return;
    d->scheduled = true;
    d->fire_at = now_seconds() + d->interval;
    g_timers.push_back(objc::retain(t));
}
void Timer_invalidate(id self, SEL) {
    auto* d = tdata(self);
    if (!d->valid) return;
    d->valid = false;
    objc::release(d->target);
    objc::release(d->user_info);
    d->target = nil;
    d->user_info = nil;
}
void Timer_dealloc(id self, SEL) {
    auto* d = tdata(self);
    if (d->valid) {
        objc::release(d->target);
        objc::release(d->user_info);
    }
    delete d;
    objc::dispose_instance(self);
}
id Timer_cls_timerWith(Class, SEL, double interval, id target, SEL sel, id user_info, BOOL repeats) {
    TRACE("NSTimer interval=%f target=%s sel=%s repeats=%d", interval, objc::class_name(target->isa), sel, repeats);
    return objc::autorelease(new_timer(interval, target, sel, user_info, repeats));
}
id Timer_cls_scheduledTimerWith(Class c, SEL s, double interval, id target, SEL sel, id user_info, BOOL repeats) {
    id t = Timer_cls_timerWith(c, s, interval, target, sel, user_info, repeats);
    schedule_timer(t);
    return t;
}
BOOL Timer_isValid(id self, SEL) { return tdata(self)->valid; }
id Timer_userInfo(id self, SEL) { return tdata(self)->user_info; }
double Timer_timeInterval(id self, SEL) { return tdata(self)->interval; }
void fire_timer(id t) {
    auto* d = tdata(t);
    if (!d->valid) return;
    void* pool = objc::pool_push();
    if (d->fn) d->fn();
    else objc::send<void>(d->target, d->sel, t);
    objc::pool_pop(pool);
}
void Timer_fire(id self, SEL) {
    fire_timer(self);
    if (!tdata(self)->repeats) Timer_invalidate(self, nullptr);
}

id RL_mainRunLoop(Class, SEL) {
    if (!g_main_runloop) g_main_runloop = objc::alloc_instance(g_NSRunLoop);
    return g_main_runloop;
}
void RL_addTimer_forMode(id, SEL, id timer, id) { schedule_timer(timer); }
void RL_run(id, SEL) { LOGW("-[NSRunLoop run] ignored (platform owns the loop)"); }

// performSelector variants
void after_delay(double delay, std::function<void()> fn) {
    id t = new_timer(delay, nil, nullptr, nil, false);
    tdata(t)->fn = std::move(fn);
    schedule_timer(t);
    objc::release(t);
}
void Obj_performSelector_afterDelay(id self, SEL, SEL sel, id arg, double delay) {
    objc::retain(self);
    objc::retain(arg);
    after_delay(delay, [self, sel, arg]() {
        objc::send<void>(self, sel, arg);
        objc::release(arg);
        objc::release(self);
    });
}
void Obj_performSelectorOnMainThread(id self, SEL, SEL sel, id arg, BOOL wait) {
    if (is_main_thread()) {
        if (wait) {
            objc::send<void>(self, sel, arg);
            return;
        }
    }
    objc::retain(self);
    objc::retain(arg);
    run_on_main(
        [self, sel, arg]() {
            objc::send<void>(self, sel, arg);
            objc::release(arg);
            objc::release(self);
        },
        wait && !is_main_thread());
}
void Obj_cancelPrevious(Class, SEL, id) {}

// ---------------------------------------------------------------------------
// Locale
// ---------------------------------------------------------------------------
id g_locale = nil;
id g_kCFLocaleLanguageCode = nil, g_kCFLocaleCountryCode = nil, g_NSLocaleIdentifier = nil;
id Locale_currentLocale(Class, SEL) {
    if (!g_locale) g_locale = objc::alloc_instance(g_NSLocale);
    return g_locale;
}
id locale_value(id key) {
    if (!key) return nil;
    std::u16string k = ns::to_u16(key);
    if (k == ns::to_u16(g_kCFLocaleLanguageCode)) return ns::str_auto(config().language);
    if (k == ns::to_u16(g_kCFLocaleCountryCode)) return ns::str_auto(config().region);
    if (k == ns::to_u16(g_NSLocaleIdentifier)) return ns::str_auto(config().language + "_" + config().region);
    return nil;
}
id Locale_objectForKey(id, SEL, id key) { return locale_value(key); }
id Locale_localeIdentifier(id, SEL) { return ns::str_auto(config().language + "_" + config().region); }
id Locale_preferredLanguages(Class, SEL) {
    return objc::autorelease(make_array({ns::str_auto(config().language)}));
}

}  // namespace

// --- public helpers ------------------------------------------------------------
double now_seconds() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

void set_main_thread() { g_main_thread = pthread_self(); }
bool is_main_thread() { return pthread_equal(pthread_self(), g_main_thread); }

void run_on_main(std::function<void()> fn, bool wait) {
    if (wait) {
        auto done = std::make_shared<std::promise<void>>();
        auto fut = done->get_future();
        {
            std::lock_guard<std::mutex> lk(g_queue_mutex);
            g_main_queue.push_back([fn, done]() {
                fn();
                done->set_value();
            });
        }
        fut.wait();
    } else {
        std::lock_guard<std::mutex> lk(g_queue_mutex);
        g_main_queue.push_back(std::move(fn));
    }
}

double runloop_run_once() {
    // queued main-thread work
    for (;;) {
        std::function<void()> fn;
        {
            std::lock_guard<std::mutex> lk(g_queue_mutex);
            if (g_main_queue.empty()) break;
            fn = std::move(g_main_queue.front());
            g_main_queue.pop_front();
        }
        void* pool = objc::pool_push();
        fn();
        objc::pool_pop(pool);
    }
    std::vector<id> due;
    double now = now_seconds();
    {
        std::lock_guard<std::recursive_mutex> lk(g_rl_mutex);
        for (id t : g_timers) {
            auto* d = tdata(t);
            if (d->valid && d->fire_at <= now) due.push_back(objc::retain(t));
        }
    }
    for (id t : due) {
        auto* d = tdata(t);
        if (d->repeats) {
            d->fire_at += d->interval;
            if (d->fire_at < now) d->fire_at = now + d->interval * 0.5;
        }
        fire_timer(t);
        if (!d->repeats) Timer_invalidate(t, nullptr);
        objc::release(t);
    }
    double next = 1.0;
    {
        std::lock_guard<std::recursive_mutex> lk(g_rl_mutex);
        now = now_seconds();
        for (size_t i = 0; i < g_timers.size();) {
            id t = g_timers[i];
            auto* d = tdata(t);
            if (!d->valid) {
                g_timers.erase(g_timers.begin() + i);
                objc::release(t);
                continue;
            }
            next = std::min(next, d->fire_at - now);
            i++;
        }
    }
    std::lock_guard<std::mutex> lk(g_queue_mutex);
    if (!g_main_queue.empty()) next = 0;
    return next < 0 ? 0 : next;
}

void post_notification(id name, id object, id user_info) {
    std::vector<Observer> matches;
    {
        std::lock_guard<std::mutex> lk(g_obs_mutex);
        for (auto& o : g_observers) {
            if (o.name && (!name || ns::to_u16(o.name) != ns::to_u16(name))) continue;
            if (o.object && o.object != object) continue;
            matches.push_back(o);
        }
    }
    id n = objc::alloc_instance(g_NSNotification);
    auto* no = (NotificationObj*)n;
    no->name = objc::retain(name);
    no->object = objc::retain(object);
    no->user_info = objc::retain(user_info);
    LOGD("post notification %s to %zu observer(s)", name ? ns::to_utf8(name).c_str() : "?", matches.size());
    for (auto& o : matches) objc::send<void>(o.observer, o.sel, n);
    objc::release(n);
}

// --- exported C functions ------------------------------------------------------
static id hle_NSSearchPathForDirectoriesInDomains(NSUInteger dir, NSUInteger, BOOL) {
    std::string p;
    switch (dir) {
        case 9: p = hle::g_documents; break;
        case 5: p = hle::g_library; break;
        case 13: p = hle::g_caches; break;
        case 14: p = hle::g_library + "/Application Support"; break;
        default: p = hle::g_library; break;
    }
    mkdirs(p);
    return objc::autorelease(make_array({ns::str_auto(p)}));
}
static id hle_NSClassFromString(id name) { return name ? (id)objc::get_class(ns::to_utf8(name).c_str()) : nil; }
static id hle_NSStringFromClass(Class c) { return c ? ns::str_auto(std::string(objc::class_name(c))) : nil; }

extern "C" void hle_NSLog_impl(id fmt, const uintptr_t* va) {
    std::string s = hle::apple_format(ns::to_utf8(fmt).c_str(), {va}, true);
    __android_log_print(ANDROID_LOG_INFO, "TS3WA-NSLog", "%s", s.c_str());
}
extern "C" void hle_NSLog(id, ...);

static id hle_CFBundleGetMainBundle() { return main_bundle(); }
static id hle_CFBundleCopyBundleURL(id b) { return objc::retain(make_file_url(bundle_data(b)->path)); }
static id hle_CFBundleCopyResourceURL(id b, id name, id type, id subdir) {
    std::string p = find_resource(bundle_data(b), ns::to_utf8(name), type ? ns::to_utf8(type) : "",
                                  subdir ? ns::to_utf8(subdir) : "");
    TRACE("CFBundleCopyResourceURL %s -> %s", ns::to_utf8(name).c_str(), p.c_str());
    return p.empty() ? nil : objc::retain(make_file_url(p));
}
static id hle_CFBundleCopyBundleLocalizations(id b) {
    std::vector<id> v;
    for (auto& l : bundle_data(b)->localizations) v.push_back(ns::str_auto(l));
    return make_array(v);
}
static id hle_CFLocaleCopyCurrent() { return objc::retain(Locale_currentLocale(nullptr, nullptr)); }
static id hle_CFLocaleGetValue(id, id key) { return locale_value(key); }
static id hle_CFLocaleCopyPreferredLanguages() { return objc::retain(Locale_preferredLanguages(nullptr, nullptr)); }
static id hle_NSTemporaryDirectory() { return ns::str_auto(config().home_path + "/tmp/"); }

static id g_NSDefaultRunLoopMode = nil, g_kCFRunLoopCommonModes = nil;

HLE_EXPORT(NSSearchPathForDirectoriesInDomains, hle_NSSearchPathForDirectoriesInDomains);
HLE_EXPORT(NSClassFromString, hle_NSClassFromString);
HLE_EXPORT(NSStringFromClass, hle_NSStringFromClass);
HLE_EXPORT(NSLog, hle_NSLog);
HLE_EXPORT(CFBundleGetMainBundle, hle_CFBundleGetMainBundle);
HLE_EXPORT(CFBundleCopyBundleURL, hle_CFBundleCopyBundleURL);
HLE_EXPORT(CFBundleCopyResourceURL, hle_CFBundleCopyResourceURL);
HLE_EXPORT(CFBundleCopyBundleLocalizations, hle_CFBundleCopyBundleLocalizations);
HLE_EXPORT(CFLocaleCopyCurrent, hle_CFLocaleCopyCurrent);
HLE_EXPORT(CFLocaleGetValue, hle_CFLocaleGetValue);
HLE_EXPORT(CFLocaleCopyPreferredLanguages, hle_CFLocaleCopyPreferredLanguages);
HLE_EXPORT(NSTemporaryDirectory, hle_NSTemporaryDirectory);
HLE_EXPORT(NSDefaultRunLoopMode, &g_NSDefaultRunLoopMode);
HLE_EXPORT(kCFRunLoopCommonModes, &g_kCFRunLoopCommonModes);
HLE_EXPORT(NSRunLoopCommonModes, &g_kCFRunLoopCommonModes);
HLE_EXPORT(NSFileSystemSize, &g_NSFileSystemSize);
HLE_EXPORT(NSFileSystemFreeSize, &g_NSFileSystemFreeSize);
HLE_EXPORT(kCFLocaleLanguageCode, &g_kCFLocaleLanguageCode);

void init_sys() {
    using objc::add_class_method;
    using objc::add_method;
    auto& c = config();
    hle::g_documents = c.home_path + "/Documents";
    hle::g_library = c.home_path + "/Library";
    hle::g_caches = c.home_path + "/Library/Caches";
    mkdirs(hle::g_documents);
    mkdirs(hle::g_caches);
    mkdirs(c.home_path + "/tmp");

    g_NSDefaultRunLoopMode = ns::make_constant("kCFRunLoopDefaultMode");
    g_kCFRunLoopCommonModes = ns::make_constant("kCFRunLoopCommonModes");
    g_NSFileSystemSize = ns::make_constant("NSFileSystemSize");
    g_NSFileSystemFreeSize = ns::make_constant("NSFileSystemFreeSize");
    g_NSFileSize = ns::make_constant("NSFileSize");
    g_kCFLocaleLanguageCode = ns::make_constant("kCFLocaleLanguageCodeKey");
    g_kCFLocaleCountryCode = ns::make_constant("kCFLocaleCountryCodeKey");
    g_NSLocaleIdentifier = ns::make_constant("kCFLocaleIdentifierKey");

    g_NSBundle = objc::define_class("NSBundle", "NSObject", 16);
    Class B = g_NSBundle;
    add_class_method(B, "mainBundle", (void*)Bundle_mainBundle);
    add_method(B, "bundlePath", (void*)Bundle_bundlePath);
    add_method(B, "bundleIdentifier", (void*)Bundle_bundleIdentifier);
    add_method(B, "infoDictionary", (void*)Bundle_infoDictionary);
    add_method(B, "objectForInfoDictionaryKey:", (void*)Bundle_objectForInfoDictionaryKey);
    add_method(B, "pathForResource:ofType:", (void*)Bundle_pathForResource_ofType);
    add_method(B, "pathForResource:ofType:inDirectory:", (void*)Bundle_pathForResource_ofType_inDirectory);
    add_method(B, "URLForResource:withExtension:", (void*)Bundle_URLForResource);
    add_method(B, "resourcePath", (void*)Bundle_resourcePath);
    add_method(B, "resourceURL", (void*)Bundle_resourceURL);
    add_method(B, "bundleURL", (void*)Bundle_bundleURL);
    add_method(B, "localizations", (void*)Bundle_localizations);
    add_method(B, "preferredLocalizations", (void*)Bundle_preferredLocalizations);
    add_method(B, "localizedStringForKey:value:table:", (void*)Bundle_localizedString);

    g_NSFileManager = objc::define_class("NSFileManager", "NSObject", 8);
    Class F = g_NSFileManager;
    add_class_method(F, "defaultManager", (void*)FM_defaultManager);
    add_method(F, "fileExistsAtPath:", (void*)FM_fileExistsAtPath);
    add_method(F, "fileExistsAtPath:isDirectory:", (void*)FM_fileExistsAtPath_isDirectory);
    add_method(F, "isReadableFileAtPath:", (void*)FM_isReadable);
    add_method(F, "isWritableFileAtPath:", (void*)FM_isWritable);
    add_method(F, "isExecutableFileAtPath:", (void*)FM_isExecutable);
    add_method(F, "isDeletableFileAtPath:", (void*)FM_isWritable);
    add_method(F, "createDirectoryAtPath:withIntermediateDirectories:attributes:error:", (void*)FM_createDirectory);
    add_method(F, "removeItemAtPath:error:", (void*)FM_removeItem);
    add_method(F, "copyItemAtPath:toPath:error:", (void*)FM_copyItem);
    add_method(F, "moveItemAtPath:toPath:error:", (void*)FM_moveItem);
    add_method(F, "contentsOfDirectoryAtPath:error:", (void*)FM_contentsOfDirectory);
    add_method(F, "directoryContentsAtPath:", (void*)FM_directoryContents);
    add_method(F, "attributesOfFileSystemForPath:error:", (void*)FM_attributesOfFileSystem);
    add_method(F, "attributesOfItemAtPath:error:", (void*)FM_attributesOfItem);
    add_method(F, "createFileAtPath:contents:attributes:", (void*)FM_createFile);
    add_method(F, "contentsAtPath:", (void*)FM_contentsAtPath);

    g_NSLock = objc::define_class("NSLock", "NSObject", 16);
    g_NSRecursiveLock = objc::define_class("NSRecursiveLock", "NSObject", 16);
    for (Class L : {g_NSLock, g_NSRecursiveLock}) {
        add_class_method(L, "alloc", (void*)Lock_alloc);
        add_method(L, "dealloc", (void*)Lock_dealloc);
        add_method(L, "lock", (void*)Lock_lock);
        add_method(L, "unlock", (void*)Lock_unlock);
        add_method(L, "tryLock", (void*)Lock_tryLock);
    }

    g_NSAutoreleasePool = objc::define_class("NSAutoreleasePool", "NSObject", 16);
    add_method(g_NSAutoreleasePool, "init", (void*)Pool_init);
    add_method(g_NSAutoreleasePool, "drain", (void*)Pool_drain);
    add_method(g_NSAutoreleasePool, "release", (void*)Pool_drain);
    add_method(g_NSAutoreleasePool, "retain", (void*)Pool_retain);

    g_NSNotificationCenter = objc::define_class("NSNotificationCenter", "NSObject", 8);
    Class NC = g_NSNotificationCenter;
    add_class_method(NC, "defaultCenter", (void*)NC_defaultCenter);
    add_method(NC, "addObserver:selector:name:object:", (void*)NC_addObserver);
    add_method(NC, "removeObserver:", (void*)NC_removeObserver);
    add_method(NC, "removeObserver:name:object:", (void*)NC_removeObserver_name_object);
    add_method(NC, "postNotificationName:object:", (void*)NC_postNotificationName_object);
    add_method(NC, "postNotificationName:object:userInfo:", (void*)NC_postNotificationName_object_userInfo);
    add_method(NC, "postNotification:", (void*)NC_postNotification);
    g_NSNotification = objc::define_class("NSNotification", "NSObject", sizeof(NotificationObj));
    add_method(g_NSNotification, "name", (void*)Notif_name);
    add_method(g_NSNotification, "object", (void*)Notif_object);
    add_method(g_NSNotification, "userInfo", (void*)Notif_userInfo);
    add_method(g_NSNotification, "dealloc", (void*)Notif_dealloc);

    g_NSTimer = objc::define_class("NSTimer", "NSObject", 16);
    Class T = g_NSTimer;
    add_class_method(T, "timerWithTimeInterval:target:selector:userInfo:repeats:", (void*)Timer_cls_timerWith);
    add_class_method(T, "scheduledTimerWithTimeInterval:target:selector:userInfo:repeats:",
                     (void*)Timer_cls_scheduledTimerWith);
    add_method(T, "invalidate", (void*)Timer_invalidate);
    add_method(T, "dealloc", (void*)Timer_dealloc);
    add_method(T, "isValid", (void*)Timer_isValid);
    add_method(T, "userInfo", (void*)Timer_userInfo);
    add_method(T, "timeInterval", (void*)Timer_timeInterval);
    add_method(T, "fire", (void*)Timer_fire);

    g_NSRunLoop = objc::define_class("NSRunLoop", "NSObject", 8);
    add_class_method(g_NSRunLoop, "mainRunLoop", (void*)RL_mainRunLoop);
    add_class_method(g_NSRunLoop, "currentRunLoop", (void*)RL_mainRunLoop);
    add_method(g_NSRunLoop, "addTimer:forMode:", (void*)RL_addTimer_forMode);
    add_method(g_NSRunLoop, "run", (void*)RL_run);

    Class O = objc::get_class("NSObject");
    add_method(O, "performSelector:withObject:afterDelay:", (void*)Obj_performSelector_afterDelay);
    add_method(O, "performSelectorOnMainThread:withObject:waitUntilDone:", (void*)Obj_performSelectorOnMainThread);
    add_class_method(O, "cancelPreviousPerformRequestsWithTarget:", (void*)Obj_cancelPrevious);

    g_NSLocale = objc::define_class("NSLocale", "NSObject", 8);
    add_class_method(g_NSLocale, "currentLocale", (void*)Locale_currentLocale);
    add_class_method(g_NSLocale, "autoupdatingCurrentLocale", (void*)Locale_currentLocale);
    add_class_method(g_NSLocale, "systemLocale", (void*)Locale_currentLocale);
    add_class_method(g_NSLocale, "preferredLanguages", (void*)Locale_preferredLanguages);
    add_method(g_NSLocale, "objectForKey:", (void*)Locale_objectForKey);
    add_method(g_NSLocale, "localeIdentifier", (void*)Locale_localeIdentifier);
}

}  // namespace foundation
