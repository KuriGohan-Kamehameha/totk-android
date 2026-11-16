// Port shim for Eden's common/stb.cpp: the one translation unit that holds the stb implementations (public domain,
// vendor/emu/externals/stb).
#define STB_IMAGE_IMPLEMENTATION 1
#define STB_IMAGE_RESIZE_IMPLEMENTATION 1
#define STB_IMAGE_WRITE_IMPLEMENTATION 1
#include "common/stb.h"
