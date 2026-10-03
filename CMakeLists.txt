cmake_minimum_required(VERSION 3.21)
project(rasm VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
option(RASM_ENABLE_AVX2 "Build the engine for AVX2-capable x86 CPUs" OFF)

if (RASM_ENABLE_AVX2)
    if (APPLE AND CMAKE_OSX_ARCHITECTURES)
        set(rasm_avx2_architecture "${CMAKE_OSX_ARCHITECTURES}")
    else()
        set(rasm_avx2_architecture "${CMAKE_SYSTEM_PROCESSOR}")
    endif()
    if (NOT rasm_avx2_architecture MATCHES "^(AMD64|amd64|x86_64|X86_64|x86|i[3-6]86)$")
        message(FATAL_ERROR "RASM_ENABLE_AVX2 requires an x86-only target architecture")
    endif()
endif()


set(RASM_SRC 
            "src/core/camera.cpp"
            "src/core/engine.cpp" 
            "src/core/entity.cpp" 
            "src/core/light.cpp"
            "src/core/material.cpp"
            "src/core/transform.cpp"
            "src/core/mesh.cpp"
            "src/core/scene.cpp"
            "src/core/renderGraph.cpp"
            "src/core/window.cpp"
            "src/core/shaderCompiler.cpp"
            "src/gfx/context.cpp"
            "src/gfx/vulkan.cpp"
            "src/core/utils.cpp"
            "src/core/handle.cpp"
            "src/core/profiler.cpp"
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

add_library(rasm_lib STATIC ${RASM_SRC})
target_link_libraries(rasm_lib PUBLIC Vulkan::Vulkan GPUOpen::VulkanMemoryAllocator)
target_link_libraries(rasm_lib PUBLIC vk-bootstrap::vk-bootstrap)
target_link_libraries(rasm_lib PUBLIC slang::slang)
target_link_libraries(rasm_lib PUBLIC glm::glm Tracy::TracyClient)
target_link_libraries(rasm_lib PUBLIC spdlog::spdlog) # make it private later, keep for debugging now
target_link_libraries(rasm_lib PRIVATE tinyobjloader::tinyobjloader)
target_link_libraries(rasm_lib PRIVATE SDL3::SDL3)

target_include_directories(rasm_lib PUBLIC ${Stb_INCLUDE_DIR} ${TINYGLTF_INCLUDE_DIRS} ${Vulkan_INCLUDE_DIRS} include)


# example executable
add_executable(Example_0 "examples/example_0/main.cpp")
add_executable(Example_1 "examples/example_1/main.cpp")
add_executable(Example_2 "examples/example_2/main.cpp")

target_link_libraries(Example_0 PUBLIC rasm_lib)
target_link_libraries(Example_1 PUBLIC rasm_lib)
target_link_libraries(Example_2 PUBLIC rasm_lib)



if (RASM_ENABLE_AVX2)
    if (MSVC)
        target_compile_options(rasm_lib PRIVATE /arch:AVX2)
    elseif (CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(rasm_lib PRIVATE -mavx2)
    else()
        message(FATAL_ERROR "RASM_ENABLE_AVX2 is not supported by this compiler")
    endif()
endif()