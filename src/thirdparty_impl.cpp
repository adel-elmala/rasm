// Single translation unit for the implementations of header-only third-party libraries.
// Every other file includes these headers without the *_IMPLEMENTATION macro.

// tinygltf must come first: its implementation includes stb_image.h and stb_image_write.h, and
// those headers expand their implementation on every include while the macro is defined.
#define TINYGLTF_IMPLEMENTATION
#include "tiny_gltf.h"

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include "stb_image_resize2.h"

#define TINYOBJLOADER_IMPLEMENTATION
#include "tiny_obj_loader.h"

// VMA_VULKAN_VERSION is deliberately left to VMA's default (derived from the Vulkan headers) so it
// matches every other translation unit that includes vk_mem_alloc.h.
#define VMA_IMPLEMENTATION
#include "vk_mem_alloc.h"
