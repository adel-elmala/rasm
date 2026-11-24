cmake_minimum_required(VERSION 3.23)

# An application that uses an installed RASM. tools/check_install.py configures it once per component, each in a build
# folder of its own: renderer and rasm are C++ and find their dependencies through vcpkg's folder; c is plain C and is
# given only RASM's install folder.
set(RASM_CONSUMER "c" CACHE STRING "The component to use: renderer, rasm or c")

if (RASM_CONSUMER STREQUAL "c")
    project(rasm_consumer LANGUAGES C)
else()
    project(rasm_consumer LANGUAGES CXX)
    set(CMAKE_CXX_STANDARD 20)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
endif()

find_package(rasm 0.1 CONFIG REQUIRED COMPONENTS ${RASM_CONSUMER})

if (RASM_CONSUMER STREQUAL "c")
    add_executable(consumer_c "main.c")
    target_link_libraries(consumer_c PRIVATE rasm::c)
    add_executable(consumer_c_renderer "main.c")
    target_link_libraries(consumer_c_renderer PRIVATE rasm::c_renderer)
elseif (RASM_CONSUMER STREQUAL "renderer")
    add_executable(consumer_renderer "renderer.cpp")
    target_link_libraries(consumer_renderer PRIVATE rasm::renderer)
else()
    add_executable(consumer_rasm "engine.cpp")
    target_link_libraries(consumer_rasm PRIVATE rasm::rasm)
endif()
