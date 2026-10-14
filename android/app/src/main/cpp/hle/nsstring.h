// NSString / CFString (toll-free bridged) helpers.
#pragma once

#include <string>

#include "hle/common.h"

namespace ns {

// Object layout shared by constant CFStrings emitted by the compiler and our
// own runtime strings.
struct StringObj {
    Class isa;
    uintptr_t info;  // constant: CF flags (0x7c8 = 8-bit, 0x7d0 = UTF-16)
    void* data;     // constant: char*/char16_t*; dynamic: StrData*
    intptr_t length;
};

bool is_string(id obj);
id make_string(const std::u16string& s);  // +1 retained
id make_string_utf8(const char* s, size_t len = std::string::npos);  // +1
id make_string_utf8(const std::string& s);  // +1
id make_constant(const char* s);  // immortal, for exported NSString constants
id autoreleased(id obj);
id str_auto(const std::string& s);  // autoreleased UTF-8 string
id str_auto(const std::u16string& s);

std::u16string to_u16(id s);
std::string to_utf8(id s);
const char* utf8_ptr(id s);  // valid while `s` lives
long length(id s);

// Mutation (dynamic strings only)
void append(id s, const std::u16string& more);
void replace(id s, long loc, long len, const std::u16string& with);

std::u16string utf8_to_u16(const std::string& s);
std::string u16_to_utf8(const std::u16string& s);

// Decode bytes in a CF/NS encoding to UTF-16. Returns false if unsupported.
bool decode_bytes(const uint8_t* bytes, size_t n, uint32_t cf_encoding, std::u16string& out);
// Encode UTF-16 into a CF/NS encoding.
bool encode_string(const std::u16string& s, uint32_t cf_encoding, std::string& out);
uint32_t ns_to_cf_encoding(NSUInteger ns_enc);

// Comparison following CFStringCompareFlags (case-insensitive=1, numeric=64...)
int compare(const std::u16string& a, const std::u16string& b, unsigned long options);

}  // namespace ns
