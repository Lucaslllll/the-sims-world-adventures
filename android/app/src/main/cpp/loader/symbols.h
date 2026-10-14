#pragma once

// Creates a callable stub that logs "UNIMPLEMENTED call: <name>" and returns 0.
void* make_unimplemented_stub(const char* name);
