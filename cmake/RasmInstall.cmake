include(CMakePackageConfigHelpers)

set(RASM_INSTALL_CONFIG_DIR "${CMAKE_INSTALL_LIBDIR}/cmake/rasm")
set(RASM_INSTALL_SHADER_DIR "${CMAKE_INSTALL_DATADIR}/rasm/shaders")

install(TARGETS rasm EXPORT rasmTargets FILE_SET HEADERS)
install(DIRECTORY "${PROJECT_SOURCE_DIR}/shaders/lib" "${PROJECT_SOURCE_DIR}/shaders/passes"
        DESTINATION "${RASM_INSTALL_SHADER_DIR}")
install(EXPORT rasmTargets NAMESPACE rasm:: DESTINATION "${RASM_INSTALL_CONFIG_DIR}")

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
export(EXPORT rasmTargets NAMESPACE rasm:: FILE "${PROJECT_BINARY_DIR}/rasmTargets.cmake")
