// The loaders' translation unit for the implementations of header-only third-party libraries, in the rasm library:
// tinygltf, tinyobjloader and stb_truetype (fonts, assets/fontLoad.cpp). tinygltf includes stb_image.h and stb_image_write.h without their implementation macros;
// their code is in the renderer's thirdparty_impl.cpp.

#define TINYGLTF_IMPLEMENTATION
#include "tiny_gltf.h"

#define TINYOBJLOADER_IMPLEMENTATION
#include "tiny_obj_loader.h"

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"
