include(CMakePackageConfigHelpers)

set(RASM_INSTALL_CONFIG_DIR "${CMAKE_INSTALL_LIBDIR}/cmake/rasm")

install(TARGETS rasm EXPORT rasmTargets ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}")
install(DIRECTORY "${PROJECT_SOURCE_DIR}/include/" DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
install(DIRECTORY "${PROJECT_SOURCE_DIR}/shaders/lib" "${PROJECT_SOURCE_DIR}/shaders/passes"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/rasm/shaders")
install(EXPORT rasmTargets NAMESPACE rasm:: DESTINATION "${RASM_INSTALL_CONFIG_DIR}")

configure_package_config_file("${PROJECT_SOURCE_DIR}/cmake/rasmConfig.cmake.in" "${PROJECT_BINARY_DIR}/rasmConfig.cmake"
                              INSTALL_DESTINATION "${RASM_INSTALL_CONFIG_DIR}")
write_basic_package_version_file("${PROJECT_BINARY_DIR}/rasmConfigVersion.cmake" COMPATIBILITY SameMinorVersion)
install(FILES "${PROJECT_BINARY_DIR}/rasmConfig.cmake" "${PROJECT_BINARY_DIR}/rasmConfigVersion.cmake"
        DESTINATION "${RASM_INSTALL_CONFIG_DIR}")

# Lets another project use a build tree directly, by pointing rasm_DIR at it.
export(EXPORT rasmTargets NAMESPACE rasm:: FILE "${PROJECT_BINARY_DIR}/rasmTargets.cmake")
