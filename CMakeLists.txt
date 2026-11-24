cmake_minimum_required(VERSION 3.23)

# Before project(): the vcpkg toolchain installs the manifest's features when project() runs.
option(RASM_IMGUI "Build with Dear ImGui: EngineConfig::ui, the engine's debug window and the application's UI" ON)
if (RASM_IMGUI)
    list(APPEND VCPKG_MANIFEST_FEATURES "imgui")
endif()

# C for the C API's example and its header checks, which compile as C.
project(rasm VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# RASM uses no C++20 modules. Scanning for them puts @modmap arguments in compile_commands.json, which break clang-tidy.
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(RASM_ENABLE_AVX2 "Build the engine for AVX2-capable x86 CPUs" OFF)
option(RASM_BUILD_EXAMPLES "Build the example applications" ${PROJECT_IS_TOP_LEVEL})
option(RASM_BUILD_TESTS "Build the unit tests" ${PROJECT_IS_TOP_LEVEL})
option(RASM_INSTALL "Generate the install and export rules" ${PROJECT_IS_TOP_LEVEL})
option(RASM_C_API "Build the C API's shared libraries, rasm_c and rasm_c_renderer (include/rasm/rasm_c.h)" ON)
set(RASM_SHADER_DIR "shaders" CACHE STRING "Default shader directory, resolved against the executable's directory if relative")
# The value ends up in a C string literal, where a Windows backslash would start an escape sequence.
cmake_path(CONVERT "${RASM_SHADER_DIR}" TO_CMAKE_PATH_LIST RASM_SHADER_DIR)

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

include(GNUInstallDirs)
include(RasmAnalysis)

add_subdirectory(src)

if (RASM_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

if (RASM_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if (RASM_INSTALL)
    include(RasmInstall)
endif()
