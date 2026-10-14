#pragma once

// Apple ARMv6 only guarantees 4-byte stack alignment. Bionic/NDK require 8.
// Wrap HLE entrypoints, preserving r0-r3 and up to 128 bytes of stack arguments.
void* arm32_wrap_function(void* function);
