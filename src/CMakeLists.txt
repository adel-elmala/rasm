# required external libs
# The Vulkan headers come from vcpkg, at the version volk is generated for, so volk, VMA, vk-bootstrap and RASM see one
# header version. Nothing links the Vulkan loader: volk loads it at run time (D24), so the SDK is not needed to build.
find_package(VulkanHeaders CONFIG REQUIRED)
# Without this, volk's config adds the SDK's headers (VULKAN_SDK) to its targets.
set(VOLK_PULL_IN_VULKAN OFF)
find_package(volk CONFIG REQUIRED)
find_package(vk-bootstrap CONFIG REQUIRED)
find_package(VulkanMemoryAllocator CONFIG REQUIRED)
find_package(glm CONFIG REQUIRED)
find_package(SDL3 CONFIG REQUIRED)
find_package(Tracy CONFIG REQUIRED)
find_package(Stb REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_path(TINYGLTF_INCLUDE_DIRS "tiny_gltf.h" REQUIRED)
find_path(EARCUT_HPP_INCLUDE_DIRS "mapbox/earcut.hpp" REQUIRED)
find_package(tinyobjloader CONFIG REQUIRED)
find_package(meshoptimizer CONFIG REQUIRED)
find_package(slang CONFIG REQUIRED)
find_package(Ktx CONFIG REQUIRED)
find_package(laszip REQUIRED)
find_package(ZLIB REQUIRED)
if (RASM_IMGUI)
    find_package(imgui CONFIG REQUIRED)
endif()

# Two libraries. rasm::renderer is the renderer alone (rasm::Renderer, RenderContext, the UI, the native and headless
# platforms, the camera and input helpers): it links no SDL3, tinygltf, tinyobjloader or libktx. rasm::rasm adds the
# all-in-one Engine, the SDL platform and the AssetLoader with the file loaders' libraries, and links rasm::renderer.
add_library(rasm_renderer STATIC)
add_library(rasm::renderer ALIAS rasm_renderer)
set_target_properties(rasm_renderer PROPERTIES EXPORT_NAME renderer)
add_library(rasm STATIC)
add_library(rasm::rasm ALIAS rasm)

# Every header in include/ is public and installed, so a new one needs no list entry. The C API's header belongs to its
# shared libraries.
file(GLOB_RECURSE RASM_PUBLIC_HEADERS CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/include/*.h")
list(FILTER RASM_PUBLIC_HEADERS EXCLUDE REGEX "/rasm_c\.h$")
target_sources(rasm_renderer PUBLIC FILE_SET HEADERS BASE_DIRS "${PROJECT_SOURCE_DIR}/include" FILES ${RASM_PUBLIC_HEADERS})

target_sources(rasm_renderer PRIVATE
    "thirdparty_impl.cpp"
    "core/cacheFile.cpp"
    "core/handle.cpp"
    "core/rangeAllocator.cpp"
    "core/threadPriority.cpp"
    "core/utils.cpp"
    "core/workerPool.cpp"
    "platform/paths.cpp"
    "platform/headlessPlatform.cpp"
    "platform/cameraInput.cpp"
    "platform/input.cpp"
    "platform/nativePlatform.cpp"
    "assets/mesh.cpp"
    "assets/meshLod.cpp"
    "assets/meshlets.cpp"
    "assets/texture.cpp"
    "assets/environment.cpp"
    "assets/material.cpp"
    "scene/scene.cpp"
    "scene/entity.cpp"
    "scene/camera.cpp"
    "scene/cameraController.cpp"
    "scene/light.cpp"
    "scene/transform.cpp"
    "scene/terrain.cpp"
    "scene/vector.cpp"
    "scene/text.cpp"
    "scene/pointCloud.cpp"
    "scene/glyphs.cpp"
    "scene/pointCloudOctree.cpp"
    "scene/colormap.cpp"
    "scene/tileResidency.cpp"
    "scene/tileStreamer.cpp"
    "renderer/blendSort.cpp"
    "renderer/bloom.cpp"
    "renderer/environmentBake.cpp"
    "renderer/frameStats.cpp"
    "renderer/geometryPool.cpp"
    "renderer/gpuScene.cpp"
    "renderer/grid.cpp"
    "renderer/lightClusters.cpp"
    "renderer/animation.cpp"
    "renderer/animationTime.cpp"
    "renderer/gpuTable.cpp"
    "renderer/labelPlacement.cpp"
    "renderer/renderGraph.cpp"
    "renderer/renderer.cpp"
    "renderer/renderSpace.cpp"
    "renderer/pipelineWarmup.cpp"
    "renderer/shaderCache.cpp"
    "renderer/shaderCompiler.cpp"
    "renderer/localShadows.cpp"
    "renderer/msaaResolve.cpp"
    "renderer/picking.cpp"
    "renderer/pointRaster.cpp"
    "renderer/glyphCull.cpp"
    "renderer/export.cpp"
    "renderer/imageWriter.cpp"
    "renderer/weightedBlend.cpp"
    "renderer/readback.cpp"
    "renderer/shadowCascades.cpp"
    "renderer/statsOverlay.cpp"
    "renderer/debugViews.cpp"
    "renderer/taa.cpp"
    "renderer/toneMap.cpp"
    "renderer/view.cpp"
    "renderer/viewGraph.cpp"
    "rhi/device.cpp"
    "rhi/memoryAliasing.cpp"
    "rhi/offscreenSwapchain.cpp"
    "rhi/mipChain.cpp"
    "rhi/renderContext.cpp"
    "rhi/resource.cpp"
    "rhi/releaseQueue.cpp"
    "rhi/textureStates.cpp"
    "rhi/uploadManager.cpp"
    "rhi/vulkan/vulkanCommandBuffer.cpp"
    "rhi/vulkan/vulkanDevice.cpp"
    "rhi/vulkan/vulkanPipelineCache.cpp"
    "rhi/vulkan/vulkanQueue.cpp"
    "rhi/vulkan/vulkanSurface.cpp"
    "rhi/vulkan/vulkanSwapchain.cpp"
    "rhi/vulkan/vulkanUtils.cpp"
)

target_sources(rasm PRIVATE
    "engine.cpp"
    "thirdparty_loaders_impl.cpp"
    "platform/sdlPlatform.cpp"
    "assets/assetLoader.cpp"
    "assets/fontLoad.cpp"
    "assets/pointCloudLoad.cpp"
    "assets/meshLoad.cpp"
    "assets/materialImport.cpp"
    "assets/textureLoad.cpp"
    "assets/environmentLoad.cpp"
    "assets/meshImport.cpp"
    "assets/gltfAccessor.cpp"
    "assets/animationImport.cpp"
    "assets/morphImport.cpp"
    "assets/skinImport.cpp"
    "assets/ktxImage.cpp"
)

# Dear ImGui: the UI renderer, the RASM window and its input from RASM's InputEvents. PUBLIC, so the application's ImGui calls use the same
# static library and context as RASM's, and RASM_IMGUI tells its code that the UI exists.
if (RASM_IMGUI)
    target_sources(rasm_renderer PRIVATE
        "platform/uiPlatform.cpp"
        "renderer/engineUi.cpp"
        "renderer/inspector.cpp"
        "renderer/ui.cpp"
    )
    target_link_libraries(rasm_renderer PUBLIC imgui::imgui)
    target_compile_definitions(rasm_renderer PUBLIC RASM_IMGUI)
endif()

# Debug and Release share one install prefix, and their libraries can't be swapped: the Windows runtimes differ.
set_target_properties(rasm_renderer rasm PROPERTIES DEBUG_POSTFIX "d")

target_compile_features(rasm_renderer PUBLIC cxx_std_20)

target_link_libraries(rasm_renderer PUBLIC glm::glm)
target_link_libraries(rasm_renderer PRIVATE Vulkan::Headers volk::volk GPUOpen::VulkanMemoryAllocator)
target_link_libraries(rasm_renderer PRIVATE vk-bootstrap::vk-bootstrap)
target_link_libraries(rasm_renderer PRIVATE slang::slang)
target_link_libraries(rasm_renderer PRIVATE meshoptimizer::meshoptimizer)
# The export writes PNG and EXR files a band at a time through deflate (src/renderer/imageWriter.cpp).
target_link_libraries(rasm_renderer PRIVATE ZLIB::ZLIB)

target_link_libraries(rasm PUBLIC rasm_renderer)
# ktxImage.cpp names Vulkan formats (vulkan_core.h); rasm_renderer's PRIVATE Vulkan::Headers does not reach rasm.
target_link_libraries(rasm PRIVATE Vulkan::Headers)
target_link_libraries(rasm PRIVATE tinyobjloader::tinyobjloader)
target_link_libraries(rasm PRIVATE SDL3::SDL3)
target_link_libraries(rasm PRIVATE KTX::ktx)
target_link_libraries(rasm PRIVATE laszip::laszip)

# Library configuration macros live here, never in a .cpp (B2). PUBLIC so every consumer that
# includes glm sees the same definitions (ODR) and gets a Vulkan [0, 1] clip-space depth range.
target_compile_definitions(rasm_renderer PUBLIC GLM_FORCE_DEPTH_ZERO_TO_ONE)
target_compile_definitions(rasm_renderer PRIVATE RASM_SHADER_DIR="${RASM_SHADER_DIR}")
# Every Vulkan function comes from volk (D24): no prototypes from vulkan.h, no global device functions (each VulkanDevice
# calls through its own table), and VMA takes its pointers from vmaImportVulkanFunctionsFromVolk.
# volk::volk is built without VK_USE_PLATFORM_* defines, and the VolkDeviceTable it fills must have RASM's layout, so
# RASM never defines them (a platform's functions would need volk::volk_headers and VOLK_IMPLEMENTATION instead).
target_compile_definitions(rasm_renderer PRIVATE VK_NO_PROTOTYPES VOLK_NO_DEVICE_PROTOTYPES VMA_STATIC_VULKAN_FUNCTIONS=0 VMA_DYNAMIC_VULKAN_FUNCTIONS=0)

# What both libraries share: the private headers under src/, stb, tinygltf and earcut (header-only: the renderer
# compiles stb_image, stb_image_write and stb_image_resize2 and the vector builders' earcut, rasm tinygltf, which calls
# stb_image), spdlog, Tracy, the flags.
foreach (rasm_target rasm_renderer rasm)
    target_link_libraries(${rasm_target} PRIVATE Tracy::TracyClient spdlog::spdlog)
    target_compile_definitions(${rasm_target} PRIVATE STBI_WINDOWS_UTF8 GLM_FORCE_EXPLICIT_CTOR)
    target_include_directories(${rasm_target} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    # SYSTEM, like the imported targets' directories, so RASM_WARNINGS does not report warnings inside these headers.
    target_include_directories(${rasm_target} SYSTEM PRIVATE ${Stb_INCLUDE_DIR} ${TINYGLTF_INCLUDE_DIRS} ${EARCUT_HPP_INCLUDE_DIRS})
    # The instrumented static library needs the sanitizer runtime in whatever links it, including find_package(rasm) consumers.
    if (RASM_SANITIZERS)
        target_link_options(${rasm_target} INTERFACE -fsanitize=${rasm_sanitizers})
    endif()
    if (RASM_ENABLE_AVX2)
        if (MSVC)
            target_compile_options(${rasm_target} PRIVATE /arch:AVX2)
        elseif (CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
            target_compile_options(${rasm_target} PRIVATE -mavx2)
        else()
            message(FATAL_ERROR "RASM_ENABLE_AVX2 is not supported by this compiler")
        endif()
    endif()
endforeach()

# The C API (include/rasm/rasm_c.h): two shared libraries for C programs and language bindings, each with a static RASM
# inside. rasm_c_renderer holds the renderer alone (rasm::renderer); rasm_c adds the Engine and the AssetLoader (rasm::rasm).
# They export only the rasm_* functions.
if (RASM_C_API)
    # Linked into the shared libraries, so their code must be position independent where that is not the default.
    set_target_properties(rasm_renderer rasm PROPERTIES POSITION_INDEPENDENT_CODE ON)

    add_library(rasm_c_objects OBJECT "c/cApi.cpp" "c/cRenderer.cpp" "c/cScene.cpp" "c/cInput.cpp" "c/cViews.cpp" "c/cAnimation.cpp" "c/cTiles.cpp" "c/cTerrain.cpp" "c/cVector.cpp" "c/cText.cpp" "c/cPicking.cpp" "c/cPointCloud.cpp" "c/cGlyphs.cpp" "c/cExport.cpp" "c/cGraph.cpp" "c/cResources.cpp")
    target_link_libraries(rasm_c_objects PRIVATE rasm_renderer spdlog::spdlog)

    add_library(rasm_c_renderer SHARED)
    add_library(rasm::c_renderer ALIAS rasm_c_renderer)
    target_link_libraries(rasm_c_renderer PRIVATE rasm_c_objects rasm_renderer)
    add_library(rasm_c SHARED "c/cLoader.cpp")
    add_library(rasm::c ALIAS rasm_c)
    target_link_libraries(rasm_c PRIVATE rasm_c_objects rasm spdlog::spdlog)
    set_target_properties(rasm_c_renderer PROPERTIES EXPORT_NAME c_renderer)
    set_target_properties(rasm_c PROPERTIES EXPORT_NAME c)

    foreach (rasm_target rasm_c_objects rasm_c_renderer rasm_c)
        target_compile_definitions(${rasm_target} PRIVATE RASM_C_BUILD GLM_FORCE_EXPLICIT_CTOR)
        target_include_directories(${rasm_target} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
        set_target_properties(${rasm_target} PROPERTIES CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON POSITION_INDEPENDENT_CODE ON)
    endforeach()
    foreach (rasm_target rasm_c_renderer rasm_c)
        target_sources(${rasm_target} PUBLIC FILE_SET HEADERS BASE_DIRS "${PROJECT_SOURCE_DIR}/include" FILES "${PROJECT_SOURCE_DIR}/include/rasm/rasm_c.h")
        set_target_properties(${rasm_target} PROPERTIES DEBUG_POSTFIX "d")
        # The static libraries inside have default visibility: only the C functions leave the library.
        if (APPLE)
            target_link_options(${rasm_target} PRIVATE "LINKER:-exported_symbol,_rasm_*")
        elseif (UNIX)
            target_link_options(${rasm_target} PRIVATE "LINKER:--exclude-libs,ALL")
        endif()
    endforeach()
endif()

# They hold only the third-party *_IMPLEMENTATION code, so clang-tidy has nothing of RASM's to check there.
set_source_files_properties("thirdparty_impl.cpp" "thirdparty_loaders_impl.cpp" PROPERTIES SKIP_LINTING ON)
# stb_image_resize2's deliberate tricks: it indexes before its sRGB table (array-bounds), reads info->user_data after
# clearing info in STBIR__FREE_AND_CLEAR (null, object-size) and copies with unaligned 8-byte stores in STBIR_MOVE_* (alignment).
# The other UBSan checks and ASan still cover the file.
if ("undefined" IN_LIST RASM_SANITIZERS)
    set_property(SOURCE "thirdparty_impl.cpp" APPEND PROPERTY COMPILE_OPTIONS "-fno-sanitize=array-bounds,null,object-size,alignment")
endif()
