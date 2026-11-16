// Port shim for Eden's common/stb.h: the same decoder selection, the vendored stb headers.
#pragma once
#define STBI_ONLY_JPEG 1
#define STBI_ONLY_PNG 1
#include <stb_image.h>
#include <stb_image_resize.h>
#include <stb_image_write.h>
