set(RASM_SRC
            "engine.cpp"
            "thirdparty_impl.cpp"
            "core/handle.cpp"
            "core/profiler.cpp"
            "core/utils.cpp"
            "platform/paths.cpp"
            "platform/window.cpp"
            "assets/mesh.cpp"
            "assets/material.cpp"
            "scene/scene.cpp"
            "scene/entity.cpp"
            "scene/camera.cpp"
            "scene/light.cpp"
            "scene/transform.cpp"
            "renderer/renderGraph.cpp"
            "renderer/shaderCompiler.cpp"
            "rhi/device.cpp"
            "rhi/renderContext.cpp"
            "rhi/resource.cpp"
            "rhi/vulkan/vulkanCommandBuffer.cpp"
            "rhi/vulkan/vulkanDevice.cpp"
            "rhi/vulkan/vulkanSwapchain.cpp"
            "rhi/vulkan/vulkanUtils.cpp"
    )

# required external libs
find_package(Vulkan REQUIRED)
find_package(vk-bootstrap CONFIG REQUIRED)
find_package(VulkanMemoryAllocator CONFIG REQUIRED)
find_package(glm CONFIG REQUIRED)
find_package(SDL3 CONFIG REQUIRED)
find_package(Tracy CONFIG REQUIRED)
find_package(Stb REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_path(TINYGLTF_INCLUDE_DIRS "tiny_gltf.h" REQUIRED)
find_package(tinyobjloader CONFIG REQUIRED)
find_package(slang CONFIG REQUIRED)

add_library(rasm STATIC ${RASM_SRC})
add_library(rasm::rasm ALIAS rasm)

# Debug and Release share one install prefix, and their libraries can't be swapped: the Windows runtimes differ.
set_target_properties(rasm PROPERTIES DEBUG_POSTFIX "d")

target_compile_features(rasm PUBLIC cxx_std_20)

target_link_libraries(rasm PUBLIC glm::glm)
target_link_libraries(rasm PRIVATE Vulkan::Vulkan GPUOpen::VulkanMemoryAllocator)
target_link_libraries(rasm PRIVATE vk-bootstrap::vk-bootstrap)
target_link_libraries(rasm PRIVATE slang::slang)
target_link_libraries(rasm PRIVATE Tracy::TracyClient)
target_link_libraries(rasm PRIVATE spdlog::spdlog)
target_link_libraries(rasm PRIVATE tinyobjloader::tinyobjloader)
target_link_libraries(rasm PRIVATE SDL3::SDL3)

# Library configuration macros live here, never in a .cpp (B2). PUBLIC so every consumer that
# includes glm sees the same definitions (ODR) and gets a Vulkan [0, 1] clip-space depth range.
target_compile_definitions(rasm PUBLIC GLM_FORCE_DEPTH_ZERO_TO_ONE)
target_compile_definitions(rasm PRIVATE RASM_SHADER_DIR="${RASM_SHADER_DIR}")
target_compile_definitions(rasm PRIVATE STBI_WINDOWS_UTF8)

target_include_directories(rasm PUBLIC "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>" "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
target_include_directories(rasm PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" ${Stb_INCLUDE_DIR} ${TINYGLTF_INCLUDE_DIRS} ${Vulkan_INCLUDE_DIRS})

if (RASM_ENABLE_AVX2)
    if (MSVC)
        target_compile_options(rasm PRIVATE /arch:AVX2)
    elseif (CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(rasm PRIVATE -mavx2)
    else()
        message(FATAL_ERROR "RASM_ENABLE_AVX2 is not supported by this compiler")
    endif()
endif()
