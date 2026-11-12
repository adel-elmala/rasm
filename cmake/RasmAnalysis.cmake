# Compiler warnings and sanitizers. Included before add_subdirectory, so they apply to RASM's own targets
# (rasm, rasm_tests, the examples) and never to the vcpkg dependencies or to a parent project.
option(RASM_WARNINGS "Build RASM's targets with extra compiler warnings" ${PROJECT_IS_TOP_LEVEL})
set(RASM_SANITIZERS "" CACHE STRING "Sanitizers to build RASM's targets with, e.g. address;undefined or thread")

if (RASM_WARNINGS)
    if (CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
        add_compile_options(/W4)
    else()
        # Initializers leave fields out on purpose, e.g. {VK_STRUCTURE_TYPE_...} or designated ones: the rest is zero or the default.
        # Sign conversions are left out: nearly all are tinygltf's int indices, and -Wsign-compare still catches the comparisons.
        add_compile_options(-Wall -Wextra -Wshadow -Wconversion -Wno-sign-conversion -Wno-missing-field-initializers)
    endif()
endif()

if (RASM_SANITIZERS)
    if (CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
        message(FATAL_ERROR "RASM_SANITIZERS needs a GNU-style compiler driver (clang++), as the presets use")
    endif()
    list(JOIN RASM_SANITIZERS "," rasm_sanitizers)
    add_compile_options(-fsanitize=${rasm_sanitizers} -fno-omit-frame-pointer -fno-sanitize-recover=all)
    add_link_options(-fsanitize=${rasm_sanitizers})

    # The Windows ASan runtime is a DLL in clang's resource directory; rasm_copy_runtime_dlls puts it next to each executable.
    if (WIN32 AND "address" IN_LIST RASM_SANITIZERS)
        execute_process(COMMAND "${CMAKE_CXX_COMPILER}" -print-file-name=clang_rt.asan_dynamic-x86_64.dll
                        OUTPUT_VARIABLE RASM_ASAN_RUNTIME_DLL OUTPUT_STRIP_TRAILING_WHITESPACE)
        if (NOT EXISTS "${RASM_ASAN_RUNTIME_DLL}")
            message(FATAL_ERROR "Could not find clang_rt.asan_dynamic-x86_64.dll next to ${CMAKE_CXX_COMPILER}")
        endif()
    endif()
endif()
