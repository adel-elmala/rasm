include(RasmRuntimeDlls)

get_property(rasm_is_multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if (rasm_is_multi_config)
    set(rasm_example_dir "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>")
else()
    set(rasm_example_dir "${CMAKE_CURRENT_BINARY_DIR}")
endif()
cmake_path(APPEND rasm_example_dir "${RASM_SHADER_DIR}" OUTPUT_VARIABLE rasm_example_shader_dir)

# Runs on every build. A POST_BUILD copy would miss shader edits, because they relink nothing.
add_custom_target(rasm_example_shaders
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${PROJECT_SOURCE_DIR}/shaders/lib" "${rasm_example_shader_dir}/lib"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${PROJECT_SOURCE_DIR}/shaders/passes" "${rasm_example_shader_dir}/passes"
    VERBATIM)

# rasm_add_example(<name>)
# Builds examples/<name>/main.cpp into an executable called <name>, linked against rasm.
function(rasm_add_example name)
    add_executable(${name} "${CMAKE_CURRENT_SOURCE_DIR}/${name}/main.cpp")
    target_link_libraries(${name} PRIVATE rasm::rasm)
    target_compile_definitions(${name} PRIVATE RASM_EXAMPLE_ASSET_ROOT="${PROJECT_SOURCE_DIR}/assets")
    add_dependencies(${name} rasm_example_shaders)
    rasm_copy_runtime_dlls(${name})
endfunction()
