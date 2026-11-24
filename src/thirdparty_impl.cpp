// The renderer's translation unit for the implementations of header-only third-party libraries: stb (images for
// readPng, writePng and the mip chains) and VMA. Every other file includes these headers without the *_IMPLEMENTATION
// macro. thirdparty_loaders_impl.cpp holds the loaders' (tinygltf, tinyobjloader), in the rasm library.

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include "stb_image_resize2.h"

// Before VMA: vk_mem_alloc.h declares vmaImportVulkanFunctionsFromVolk only after volk.h.
#include "volk.h"

// VMA_VULKAN_VERSION is deliberately left to VMA's default (derived from the Vulkan headers) so it
// matches every other translation unit that includes vk_mem_alloc.h.
#define VMA_IMPLEMENTATION
#include "vk_mem_alloc.h"
