// Foundation helpers shared between HLE modules.
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "hle/common.h"

namespace foundation {

void init();          // defines all Foundation classes (call before loading the image)
void init_strings();  // NSString family (called from init)

// NSArray / NSMutableArray
id make_mutable_array();  // +1
id make_array(const std::vector<id>& items);  // +1
void array_add(id arr, id obj);
std::vector<id>& array_items(id arr);

// NSDictionary / NSMutableDictionary
id make_mutable_dictionary();  // +1
void dict_set(id dict, id key, id value);
id dict_get(id dict, id key);
id dict_get(id dict, const char* key);

// NSSet (used for touches)
id make_set(const std::vector<id>& items);  // +1

// NSNumber
id make_number_bool(bool v);      // autoreleased
id make_number_int(long long v);  // autoreleased
id make_number_double(double v);  // autoreleased

// NSData
id make_data(const void* bytes, size_t n);  // +1
const void* data_bytes(id data);
size_t data_length(id data);

// NSCharacterSet
bool charset_contains(id set, char16_t c);

// NSURL
id make_file_url(const std::string& path);  // autoreleased
std::string url_path(id url);
std::string url_string(id url);

// Property lists (binary and XML) -> Foundation objects (+1)
id parse_plist(const std::vector<uint8_t>& data);

// Run loop -------------------------------------------------------------------
// Runs due timers and queued main-thread calls. Returns seconds until the next
// timer is due (or a large value if none).
double runloop_run_once();
// Queue a call on the main thread; if `wait`, block until it ran.
void run_on_main(std::function<void()> fn, bool wait);
bool is_main_thread();
void set_main_thread();
double now_seconds();

// Notifications
void post_notification(id name, id object, id user_info = nil);

}  // namespace foundation
