include(RasmRuntimeDlls)

# rasm_add_example(<name> [<library>])
# Builds examples/<name>/main.cpp into an executable called <name>, linked against rasm::rasm, or the library given
# (rasm::renderer for the renderer alone).
function(rasm_add_example name)
    set(library rasm::rasm)
    if (ARGC GREATER 1)
        set(library ${ARGV1})
    endif()
    add_executable(${name} "${CMAKE_CURRENT_SOURCE_DIR}/${name}/main.cpp")
    target_link_libraries(${name} PRIVATE ${library})
    # exampleArgs.h, the run-control flags every example takes.
    target_include_directories(${name} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_compile_definitions(${name} PRIVATE RASM_EXAMPLE_ASSET_ROOT="${PROJECT_SOURCE_DIR}/assets"
                                               RASM_EXAMPLE_SHADER_DIR="${PROJECT_SOURCE_DIR}/shaders")
    rasm_copy_runtime_dlls(${name})
endfunction()
