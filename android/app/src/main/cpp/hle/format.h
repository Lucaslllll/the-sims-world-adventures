// printf-style formatting for arguments passed with the Apple arm64 variadic
// convention: every variadic argument sits in its own 8-byte stack slot, so
// an Apple va_list is just a pointer to those slots.
#pragma once

#include <cstdint>
#include <string>

#include "hle/common.h"

namespace hle {

struct AppleArgs {
    const uintptr_t* slots;
};

// `allow_objects` enables %@ (NSString/CFString formats).
std::string apple_format(const char* fmt, AppleArgs args, bool allow_objects);

}  // namespace hle
