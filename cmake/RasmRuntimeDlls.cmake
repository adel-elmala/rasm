# rasm_copy_runtime_dlls(<target>)
# Copies the DLLs an executable needs next to it, so it runs without touching PATH.
function(rasm_copy_runtime_dlls target)
    # vcpkg's app-local copy misses TracyClient.dll in Debug: the port installs it to debug/bin/Debug, not debug/bin.
    if (WIN32)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_RUNTIME_DLLS:${target}> $<TARGET_FILE_DIR:${target}>
            COMMAND_EXPAND_LISTS)
        if (RASM_ASAN_RUNTIME_DLL)
            add_custom_command(TARGET ${target} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different "${RASM_ASAN_RUNTIME_DLL}" $<TARGET_FILE_DIR:${target}>)
        endif()
    endif()
endfunction()
