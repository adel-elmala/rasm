set(RASM_SRC
            "engine.cpp"
            "thirdparty_impl.cpp"
            "core/handle.cpp"
            "core/profiler.cpp"
            "core/utils.cpp"
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
            "rhi/renderContext.cpp"
            "rhi/vulkan/vulkanContext.cpp"
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

target_link_libraries(rasm PUBLIC Vulkan::Vulkan GPUOpen::VulkanMemoryAllocator)
target_link_libraries(rasm PUBLIC vk-bootstrap::vk-bootstrap)
target_link_libraries(rasm PUBLIC slang::slang)
target_link_libraries(rasm PUBLIC glm::glm Tracy::TracyClient)
target_link_libraries(rasm PUBLIC spdlog::spdlog) # make it private later, keep for debugging now
target_link_libraries(rasm PRIVATE tinyobjloader::tinyobjloader)
target_link_libraries(rasm PRIVATE SDL3::SDL3)

# Library configuration macros live here, never in a .cpp (B2). PUBLIC so every consumer that
# includes glm sees the same definitions (ODR) and gets a Vulkan [0, 1] clip-space depth range.
target_compile_definitions(rasm PUBLIC GLM_FORCE_DEPTH_ZERO_TO_ONE)

target_include_directories(rasm PUBLIC ${Stb_INCLUDE_DIR} ${TINYGLTF_INCLUDE_DIRS} ${Vulkan_INCLUDE_DIRS} "${PROJECT_SOURCE_DIR}/include")
# TODO(D2): src/ is public only because public headers still include private ones
# (renderer/shaderCompiler.h, renderer/uberMaterialTypes.h, rhi/vulkan/*). Make it PRIVATE once they don't.
target_include_directories(rasm PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")

if (RASM_ENABLE_AVX2)
    if (MSVC)
        target_compile_options(rasm PRIVATE /arch:AVX2)
    elseif (CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(rasm PRIVATE -mavx2)
    else()
        message(FATAL_ERROR "RASM_ENABLE_AVX2 is not supported by this compiler")
    endif()
endif()
