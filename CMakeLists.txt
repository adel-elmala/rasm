cmake_minimum_required(VERSION 3.20)
project(rasm VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)

set(RASM_SRC 
            "src/core/camera.cpp"
            "src/core/engine.cpp" 
            "src/core/entity.cpp" 
            "src/core/light.cpp"
            "src/core/material.cpp"
            "src/core/math.cpp"
            "src/core/mesh.cpp"
            "src/core/rasm.cpp" 
            "src/core/scene.cpp"
            "src/core/renderGraph.cpp"
            "src/core/window.cpp"
            "src/core/shaderCompiler.cpp"
            "src/gfx/context.cpp"
            "src/gfx/vulkan.cpp"
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
find_path(TINYGLTF_INCLUDE_DIRS "tiny_gltf.h")
find_package(tinyobjloader CONFIG REQUIRED)
find_package(slang CONFIG REQUIRED)


add_library(rasm_lib STATIC ${RASM_SRC})
target_link_libraries(rasm_lib PRIVATE glm::glm Tracy::TracyClient SDL3::SDL3) 
target_link_libraries(rasm_lib PUBLIC Vulkan::Vulkan GPUOpen::VulkanMemoryAllocator)
target_link_libraries(rasm_lib PRIVATE vk-bootstrap::vk-bootstrap vk-bootstrap::vk-bootstrap-compiler-warnings)
target_link_libraries(rasm_lib PRIVATE  tinyobjloader::tinyobjloader)
target_link_libraries(rasm_lib PUBLIC  spdlog::spdlog) # make it private later, keep for debugging now
target_link_libraries(rasm_lib PRIVATE slang::slang)

target_include_directories(rasm_lib PUBLIC ${Stb_INCLUDE_DIR} ${TINYGLTF_INCLUDE_DIRS} ${Vulkan_INCLUDE_DIRS} include)


# example executable
add_executable(Example_0 "examples/example_0/main.cpp")
add_executable(Example_1 "examples/example_1/main.cpp")

target_link_libraries(Example_0 PUBLIC
rasm_lib
)

target_link_libraries(Example_1 PUBLIC
rasm_lib
)

if (MSVC)
    add_compile_options(/arch:AVX2)
endif()