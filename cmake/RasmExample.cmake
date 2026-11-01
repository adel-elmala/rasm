# rasm_add_example(<name>)
# Builds examples/<name>/main.cpp into an executable called <name>, linked against rasm.
function(rasm_add_example name)
    add_executable(${name} "${CMAKE_CURRENT_SOURCE_DIR}/${name}/main.cpp")
    target_link_libraries(${name} PRIVATE rasm::rasm)

    # vcpkg's app-local copy misses TracyClient.dll in Debug: the port installs it to debug/bin/Debug, not debug/bin.
    if (WIN32)
        add_custom_command(TARGET ${name} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_RUNTIME_DLLS:${name}> $<TARGET_FILE_DIR:${name}>
            COMMAND_EXPAND_LISTS)
    endif()
endfunction()
