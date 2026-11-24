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
find_package(tinyobjloader CONFIG REQUIRED)
find_package(meshoptimizer CONFIG REQUIRED)
find_package(slang CONFIG REQUIRED)
find_package(Ktx CONFIG REQUIRED)
if (RASM_IMGUI)
    find_package(imgui CONFIG REQUIRED)
endif()

add_library(rasm STATIC)
add_library(rasm::rasm ALIAS rasm)

# Every header in include/ is public and installed, so a new one needs no list entry.
file(GLOB_RECURSE RASM_PUBLIC_HEADERS CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/include/*.h")
target_sources(rasm PUBLIC FILE_SET HEADERS BASE_DIRS "${PROJECT_SOURCE_DIR}/include" FILES ${RASM_PUBLIC_HEADERS})

target_sources(rasm PRIVATE
    "engine.cpp"
    "thirdparty_impl.cpp"
    "core/cacheFile.cpp"
    "core/handle.cpp"
    "core/rangeAllocator.cpp"
    "core/utils.cpp"
    "core/workerPool.cpp"
    "platform/paths.cpp"
    "platform/headlessPlatform.cpp"
    "platform/cameraInput.cpp"
    "platform/input.cpp"
    "platform/nativePlatform.cpp"
    "platform/sdlPlatform.cpp"
    "assets/mesh.cpp"
    "assets/meshImport.cpp"
    "assets/gltfAccessor.cpp"
    "assets/animationImport.cpp"
    "assets/morphImport.cpp"
    "assets/skinImport.cpp"
    "assets/meshLod.cpp"
    "assets/meshlets.cpp"
    "assets/ktxImage.cpp"
    "assets/texture.cpp"
    "assets/environment.cpp"
    "assets/material.cpp"
    "scene/scene.cpp"
    "scene/entity.cpp"
    "scene/camera.cpp"
    "scene/cameraController.cpp"
    "scene/light.cpp"
    "scene/transform.cpp"
    "renderer/blendSort.cpp"
    "renderer/bloom.cpp"
    "renderer/environmentBake.cpp"
    "renderer/frameStats.cpp"
    "renderer/geometryPool.cpp"
    "renderer/gpuScene.cpp"
    "renderer/lightClusters.cpp"
    "renderer/animation.cpp"
    "renderer/animationTime.cpp"
    "renderer/gpuTable.cpp"
    "renderer/renderGraph.cpp"
    "renderer/renderer.cpp"
    "renderer/shaderCache.cpp"
    "renderer/shaderCompiler.cpp"
    "renderer/localShadows.cpp"
    "renderer/msaaResolve.cpp"
    "renderer/readback.cpp"
    "renderer/shadowCascades.cpp"
    "renderer/statsOverlay.cpp"
    "renderer/debugViews.cpp"
    "renderer/taa.cpp"
    "renderer/toneMap.cpp"
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

# Dear ImGui: the UI renderer, the RASM window and its input from RASM's InputEvents. PUBLIC, so the application's ImGui calls use the same
# static library and context as RASM's, and RASM_IMGUI tells its code that the UI exists.
if (RASM_IMGUI)
    target_sources(rasm PRIVATE
        "platform/uiPlatform.cpp"
        "renderer/engineUi.cpp"
        "renderer/inspector.cpp"
        "renderer/ui.cpp"
    )
    target_link_libraries(rasm PUBLIC imgui::imgui)
    target_compile_definitions(rasm PUBLIC RASM_IMGUI)
endif()

# Debug and Release share one install prefix, and their libraries can't be swapped: the Windows runtimes differ.
set_target_properties(rasm PROPERTIES DEBUG_POSTFIX "d")

target_compile_features(rasm PUBLIC cxx_std_20)

target_link_libraries(rasm PUBLIC glm::glm)
target_link_libraries(rasm PRIVATE Vulkan::Headers volk::volk GPUOpen::VulkanMemoryAllocator)
target_link_libraries(rasm PRIVATE vk-bootstrap::vk-bootstrap)
target_link_libraries(rasm PRIVATE slang::slang)
target_link_libraries(rasm PRIVATE Tracy::TracyClient)
target_link_libraries(rasm PRIVATE spdlog::spdlog)
target_link_libraries(rasm PRIVATE tinyobjloader::tinyobjloader)
target_link_libraries(rasm PRIVATE meshoptimizer::meshoptimizer)
target_link_libraries(rasm PRIVATE SDL3::SDL3)
target_link_libraries(rasm PRIVATE KTX::ktx)

# Library configuration macros live here, never in a .cpp (B2). PUBLIC so every consumer that
# includes glm sees the same definitions (ODR) and gets a Vulkan [0, 1] clip-space depth range.
target_compile_definitions(rasm PUBLIC GLM_FORCE_DEPTH_ZERO_TO_ONE)
target_compile_definitions(rasm PRIVATE RASM_SHADER_DIR="${RASM_SHADER_DIR}")
target_compile_definitions(rasm PRIVATE STBI_WINDOWS_UTF8)
# Every Vulkan function comes from volk (D24): no prototypes from vulkan.h, no global device functions (each VulkanDevice
# calls through its own table), and VMA takes its pointers from vmaImportVulkanFunctionsFromVolk.
# volk::volk is built without VK_USE_PLATFORM_* defines, and the VolkDeviceTable it fills must have RASM's layout, so
# RASM never defines them (a platform's functions would need volk::volk_headers and VOLK_IMPLEMENTATION instead).
target_compile_definitions(rasm PRIVATE VK_NO_PROTOTYPES VOLK_NO_DEVICE_PROTOTYPES VMA_STATIC_VULKAN_FUNCTIONS=0 VMA_DYNAMIC_VULKAN_FUNCTIONS=0)

target_include_directories(rasm PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
# SYSTEM, like the imported targets' directories, so RASM_WARNINGS does not report warnings inside these headers.
target_include_directories(rasm SYSTEM PRIVATE ${Stb_INCLUDE_DIR} ${TINYGLTF_INCLUDE_DIRS})

# Holds only the third-party *_IMPLEMENTATION code, so clang-tidy has nothing of RASM's to check there.
set_source_files_properties("thirdparty_impl.cpp" PROPERTIES SKIP_LINTING ON)
# stb_image_resize2's deliberate tricks: it indexes before its sRGB table (array-bounds), reads info->user_data after
# clearing info in STBIR__FREE_AND_CLEAR (null, object-size) and copies with unaligned 8-byte stores in STBIR_MOVE_* (alignment).
# The other UBSan checks and ASan still cover the file.
if ("undefined" IN_LIST RASM_SANITIZERS)
    set_property(SOURCE "thirdparty_impl.cpp" APPEND PROPERTY COMPILE_OPTIONS "-fno-sanitize=array-bounds,null,object-size,alignment")
endif()

# The instrumented static library needs the sanitizer runtime in whatever links it, including find_package(rasm) consumers.
if (RASM_SANITIZERS)
    target_link_options(rasm INTERFACE -fsanitize=${rasm_sanitizers})
endif()

if (RASM_ENABLE_AVX2)
    if (MSVC)
        target_compile_options(rasm PRIVATE /arch:AVX2)
    elseif (CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(rasm PRIVATE -mavx2)
    else()
        message(FATAL_ERROR "RASM_ENABLE_AVX2 is not supported by this compiler")
    endif()
endif()
