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
    )


# required external libs
find_package(Vulkan REQUIRED)
find_package(glm CONFIG REQUIRED)
find_package(SDL3 CONFIG REQUIRED)
find_package(Tracy CONFIG REQUIRED)
find_package(Stb REQUIRED)



add_library(rasm_lib STATIC ${RASM_SRC})
target_link_libraries(rasm_lib PUBLIC glm::glm Vulkan::Vulkan Tracy::TracyClient) 
target_include_directories(rasm_lib PRIVATE ${Stb_INCLUDE_DIR} PUBLIC include)


# exmaple executable
add_executable(Example_0 "examples/example_0/main.cpp")
add_executable(Example_1 "examples/example_1/main.cpp")

target_link_libraries(Example_0 PUBLIC
$<TARGET_NAME_IF_EXISTS:SDL3::SDL3main> # SDL3
$<IF:$<TARGET_EXISTS:SDL3::SDL3>,SDL3::SDL3,SDL3::SDL3-static>
rasm_lib
)

target_link_libraries(Example_1 PUBLIC
$<TARGET_NAME_IF_EXISTS:SDL3::SDL3main> # SDL3
$<IF:$<TARGET_EXISTS:SDL3::SDL3>,SDL3::SDL3,SDL3::SDL3-static>
rasm_lib
)


add_definitions(/arch:AVX2)
