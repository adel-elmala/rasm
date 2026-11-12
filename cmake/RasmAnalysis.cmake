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
    # LLVM names it clang_rt.asan_dynamic-<arch>.dll, or clang_rt.asan_dynamic.dll in a per-target runtime folder.
    if (WIN32 AND "address" IN_LIST RASM_SANITIZERS)
        if (CMAKE_SYSTEM_PROCESSOR MATCHES "ARM64|aarch64")
            set(rasm_asan_arch "aarch64")
        else()
            set(rasm_asan_arch "x86_64")
        endif()
        foreach (rasm_asan_name "clang_rt.asan_dynamic-${rasm_asan_arch}.dll" "clang_rt.asan_dynamic.dll")
            execute_process(COMMAND "${CMAKE_CXX_COMPILER}" -print-file-name=${rasm_asan_name}
                            OUTPUT_VARIABLE rasm_asan_dll OUTPUT_STRIP_TRAILING_WHITESPACE)
            if (EXISTS "${rasm_asan_dll}")
                set(RASM_ASAN_RUNTIME_DLL "${rasm_asan_dll}")
                break()
            endif()
        endforeach()
        if (NOT RASM_ASAN_RUNTIME_DLL)
            message(WARNING "Could not find the ASan runtime DLL of ${CMAKE_CXX_COMPILER}; put it on PATH to run the executables")
        endif()
    endif()
endif()

# SKIP_LINTING, which keeps clang-tidy off src/thirdparty_impl.cpp, needs CMake 3.27.
if (CMAKE_CXX_CLANG_TIDY AND CMAKE_VERSION VERSION_LESS 3.27)
    message(FATAL_ERROR "clang-tidy builds need CMake 3.27 or newer (this is ${CMAKE_VERSION})")
endif()
