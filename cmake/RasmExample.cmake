include(RasmRuntimeDlls)

# rasm_add_example(<name>)
# Builds examples/<name>/main.cpp into an executable called <name>, linked against rasm.
function(rasm_add_example name)
    add_executable(${name})
    target_sources(${name} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/${name}/main.cpp")
    target_link_libraries(${name} PRIVATE rasm::rasm)
    target_compile_definitions(${name} PRIVATE RASM_EXAMPLE_ASSET_ROOT="${PROJECT_SOURCE_DIR}/assets"
                                               RASM_EXAMPLE_SHADER_DIR="${PROJECT_SOURCE_DIR}/shaders")
    rasm_copy_runtime_dlls(${name})
endfunction()
