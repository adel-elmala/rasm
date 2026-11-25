include(RasmRuntimeDlls)

# rasm_add_example(<name> [<library>])
# Builds examples/<name>/main.cpp, or main.c for a C program, into an executable called <name>, linked against
# rasm::rasm, or the library given (rasm::renderer for the renderer alone, rasm::c for the C API).
function(rasm_add_example name)
    set(library rasm::rasm)
    if (ARGC GREATER 1)
        set(library ${ARGV1})
    endif()
    set(source "${CMAKE_CURRENT_SOURCE_DIR}/${name}/main.cpp")
    if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${name}/main.c")
        set(source "${CMAKE_CURRENT_SOURCE_DIR}/${name}/main.c")
    endif()
    add_executable(${name} "${source}")
    target_link_libraries(${name} PRIVATE ${library})
    # exampleArgs.h, the run-control flags every example takes.
    target_include_directories(${name} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_compile_definitions(${name} PRIVATE RASM_EXAMPLE_ASSET_ROOT="${PROJECT_SOURCE_DIR}/assets"
                                               RASM_EXAMPLE_SHADER_DIR="${PROJECT_SOURCE_DIR}/shaders")
    rasm_copy_runtime_dlls(${name})
endfunction()
