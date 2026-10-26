# rasm_add_example(<name>)
# Builds examples/<name>/main.cpp into an executable called <name>, linked against rasm.
function(rasm_add_example name)
    add_executable(${name} "${CMAKE_CURRENT_SOURCE_DIR}/${name}/main.cpp")
    target_link_libraries(${name} PRIVATE rasm::rasm)
endfunction()
