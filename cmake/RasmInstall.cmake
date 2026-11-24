include(CMakePackageConfigHelpers)

set(RASM_INSTALL_CONFIG_DIR "${CMAKE_INSTALL_LIBDIR}/cmake/rasm")
set(RASM_INSTALL_SHADER_DIR "${CMAKE_INSTALL_DATADIR}/rasm/shaders")

# Two export sets, so a renderer-only consumer (find_package(rasm COMPONENTS renderer)) loads rasm::renderer and needs
# none of rasm::rasm's dependencies.
install(TARGETS rasm_renderer EXPORT rasmRendererTargets FILE_SET HEADERS)
install(TARGETS rasm EXPORT rasmTargets)
install(DIRECTORY "${PROJECT_SOURCE_DIR}/shaders/lib" "${PROJECT_SOURCE_DIR}/shaders/passes"
        DESTINATION "${RASM_INSTALL_SHADER_DIR}")
install(EXPORT rasmRendererTargets NAMESPACE rasm:: DESTINATION "${RASM_INSTALL_CONFIG_DIR}")
install(EXPORT rasmTargets NAMESPACE rasm:: DESTINATION "${RASM_INSTALL_CONFIG_DIR}")

# The C API's shared libraries (find_package(rasm COMPONENTS c)), with what they need at run time beside them: the
# shaders, which a relative shaderDir finds next to the library under any host program (python.exe), and on Windows the
# dependencies' DLLs. A C program needs nothing else, not even vcpkg.
if (RASM_C_API)
    # The vcpkg shared libraries they load, and the ones those load (zstd for libktx on Windows, Slang's compiler on
    # macOS too), from the build's configuration, go beside them. The Debug TracyClient.dll is in debug/bin/Debug (see
    # RasmRuntimeDlls.cmake). On macOS and Linux the libraries look beside themselves (@loader_path, $ORIGIN).
    set(rasm_vcpkg_dir "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}")
    if (CMAKE_BUILD_TYPE STREQUAL "Debug")
        set(rasm_vcpkg_dir "${rasm_vcpkg_dir}/debug")
    endif()
    if (WIN32)
        set(rasm_c_library_dir "${CMAKE_INSTALL_BINDIR}")
        set(rasm_vcpkg_libraries "${rasm_vcpkg_dir}/bin" "${rasm_vcpkg_dir}/bin/Debug")
        set(rasm_system_libraries "[Ss]ystem32")
    else()
        set(rasm_c_library_dir "${CMAKE_INSTALL_LIBDIR}")
        set(rasm_vcpkg_libraries "${rasm_vcpkg_dir}/lib")
        set(rasm_system_libraries "^/usr/lib" "^/System/" "^/lib")
        if (APPLE)
            set_target_properties(rasm_c_renderer rasm_c PROPERTIES INSTALL_RPATH "@loader_path")
        else()
            set_target_properties(rasm_c_renderer rasm_c PROPERTIES INSTALL_RPATH "$ORIGIN")
        endif()
    endif()
    install(TARGETS rasm_c_renderer rasm_c EXPORT rasmCTargets
            RUNTIME_DEPENDENCIES
                # The system's own: by name before they are looked for (macOS keeps them in the dyld cache, not on
                # disk), and by the folder they were found in.
                PRE_EXCLUDE_REGEXES "^api-ms-" "^ext-ms-" "^/usr/lib/" "^/System/"
                POST_EXCLUDE_REGEXES ${rasm_system_libraries}
                DIRECTORIES ${rasm_vcpkg_libraries}
            FRAMEWORK DESTINATION "${CMAKE_INSTALL_LIBDIR}"
            FILE_SET HEADERS)
    install(EXPORT rasmCTargets NAMESPACE rasm:: DESTINATION "${RASM_INSTALL_CONFIG_DIR}")
    # An absolute RASM_SHADER_DIR is where the libraries look, wherever they are: nothing goes beside them then.
    if (NOT IS_ABSOLUTE "${RASM_SHADER_DIR}")
        install(DIRECTORY "${PROJECT_SOURCE_DIR}/shaders/lib" "${PROJECT_SOURCE_DIR}/shaders/passes"
                DESTINATION "${rasm_c_library_dir}/${RASM_SHADER_DIR}")
    endif()
endif()

# The install prefix and the build tree each get a config, because their shaders are in different places.
set(RASM_PACKAGE_SHADER_DIR "${RASM_INSTALL_SHADER_DIR}")
configure_package_config_file("${PROJECT_SOURCE_DIR}/cmake/rasmConfig.cmake.in" "${PROJECT_BINARY_DIR}/install/rasmConfig.cmake"
                              INSTALL_DESTINATION "${RASM_INSTALL_CONFIG_DIR}"
                              PATH_VARS RASM_PACKAGE_SHADER_DIR)
write_basic_package_version_file("${PROJECT_BINARY_DIR}/rasmConfigVersion.cmake" COMPATIBILITY SameMinorVersion)
install(FILES "${PROJECT_BINARY_DIR}/install/rasmConfig.cmake" "${PROJECT_BINARY_DIR}/rasmConfigVersion.cmake"
        DESTINATION "${RASM_INSTALL_CONFIG_DIR}")

# Lets another project use a build tree directly, by pointing rasm_DIR at it.
set(RASM_PACKAGE_SHADER_DIR "${PROJECT_SOURCE_DIR}/shaders")
configure_package_config_file("${PROJECT_SOURCE_DIR}/cmake/rasmConfig.cmake.in" "${PROJECT_BINARY_DIR}/rasmConfig.cmake"
                              INSTALL_DESTINATION "${PROJECT_BINARY_DIR}"
                              INSTALL_PREFIX "${PROJECT_BINARY_DIR}"
                              PATH_VARS RASM_PACKAGE_SHADER_DIR)
export(EXPORT rasmRendererTargets NAMESPACE rasm:: FILE "${PROJECT_BINARY_DIR}/rasmRendererTargets.cmake")
export(EXPORT rasmTargets NAMESPACE rasm:: FILE "${PROJECT_BINARY_DIR}/rasmTargets.cmake")
if (RASM_C_API)
    export(EXPORT rasmCTargets NAMESPACE rasm:: FILE "${PROJECT_BINARY_DIR}/rasmCTargets.cmake")
endif()
