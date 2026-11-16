// Port shim in front of the system's <zlib.h> (found first because shim/ leads the include path). The NDK's zconf.h
// defines the DOS-era macro `far` to nothing; the Eden Duo runtime has a local variable of that name
// (mod_input_swipe.h), which a translation unit that includes zlib first (engine_mercury.cpp) then cannot compile.
// Eden builds against its own zlib, which does not leak the macro.
#pragma once
#include_next <zlib.h>
#undef far
#undef near
