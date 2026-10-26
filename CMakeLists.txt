cmake_minimum_required(VERSION 3.21)
project(rasm VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(RASM_ENABLE_AVX2 "Build the engine for AVX2-capable x86 CPUs" OFF)
option(RASM_BUILD_EXAMPLES "Build the example applications" ${PROJECT_IS_TOP_LEVEL})

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

list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")

add_subdirectory(src)

if (RASM_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()
