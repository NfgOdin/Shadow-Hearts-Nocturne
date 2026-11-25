// Single-TU C++ unity build of official Lua 5.4.7 (MIT License) for Shadow Hearts: Nocturne.
// Compiled as C++20 so builds never require a separate C compiler and Lua errors unwind cleanly via C++ exceptions.

#define LUA_CORE
#define LUA_LIB
#define ltable_c
#define lvm_c

#include "luaconf.h"

// Core Lua 5.4.7 VM
#include "lzio.c"
#include "lctype.c"
#include "lopcodes.c"
#include "lmem.c"
#include "lundump.c"
#include "ldump.c"
#include "lstate.c"
#include "lgc.c"
#include "llex.c"
#include "lcode.c"
#include "lparser.c"
#include "ldebug.c"
#include "lfunc.c"
#include "lobject.c"
#include "ltm.c"
#include "lstring.c"
#include "ltable.c"
#include "ldo.c"
#include "lvm.c"
#include "lapi.c"

// Auxiliary & Safe Standard Libraries (os/io/debug/loadlib intentionally omitted from sandbox)
#include "lauxlib.c"
#include "lbaselib.c"
#include "lcorolib.c"
#include "lmathlib.c"
#include "lstrlib.c"
#include "ltablib.c"
#include "lutf8lib.c"
