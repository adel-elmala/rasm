include(RasmRuntimeDlls)

find_package(doctest CONFIG REQUIRED)
find_package(glm CONFIG REQUIRED)
find_package(Ktx CONFIG REQUIRED)

add_executable(rasm_tests
            "main.cpp"
            "animationTests.cpp"
            "blendSortTests.cpp"
            "cApiHandleTests.cpp"
            "bloomTests.cpp"
            "cacheFileTests.cpp"
            "cameraTests.cpp"
            "environmentTests.cpp"
            "gltfImportTests.cpp"
            "gpuTableTests.cpp"
            "handleTests.cpp"
            "importMeshText.cpp"
            "inputTests.cpp"
            "inspectorTests.cpp"
            "ktxImageTests.cpp"
            "lightClustersTests.cpp"
            "localShadowsTests.cpp"
            "memoryAliasingTests.cpp"
            "meshLodTests.cpp"
            "meshletTests.cpp"
            "meshTests.cpp"
            "morphTargetsTests.cpp"
            "pathTests.cpp"
            "rangeAllocatorTests.cpp"
            "readbackTests.cpp"
            "releaseQueueTests.cpp"
            "rendererConfigTests.cpp"
            "renderSpaceTests.cpp"
            "renderGraphTests.cpp"
            "resourceDescTests.cpp"
            "shaderCacheTests.cpp"
            "shadowCascadesTests.cpp"
            "skinningTests.cpp"
            "taaTests.cpp"
            "textureStatesTests.cpp"
            "textureUploadTests.cpp"
            "terrainTests.cpp"
            "vectorTests.cpp"
            "tileResidencyTests.cpp"
            "transformTests.cpp"
            "uiTests.cpp"
            "uploadManagerTests.cpp"
            "viewTests.cpp"
            "workerPoolTests.cpp"
    )

# The KTX tests encode small Basis images in memory with libktx, so they need no test files.
target_link_libraries(rasm_tests PRIVATE rasm::rasm doctest::doctest KTX::ktx)
# Private classes such as rhi::UploadManager are tested against a fake rhi::Device.
target_include_directories(rasm_tests PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_compile_definitions(rasm_tests PRIVATE RASM_TEST_EXECUTABLE_DIR="$<TARGET_FILE_DIR:rasm_tests>"
                                              RASM_TEST_DEFAULT_SHADER_DIR="${RASM_SHADER_DIR}")
rasm_copy_runtime_dlls(rasm_tests)

add_test(NAME rasm_tests COMMAND rasm_tests)

# The C API as a binding uses it: rasm_c_tests links only the rasm_c shared library (the C++ headers give the defaults to
# compare with), and the header checks are C programs, built as strict C99 and C11 against rasm_c_renderer.
if (RASM_C_API)
    add_executable(rasm_c_tests "main.cpp" "cApiTests.cpp")
    target_link_libraries(rasm_c_tests PRIVATE rasm::c doctest::doctest glm::glm)
    target_include_directories(rasm_c_tests PRIVATE "${PROJECT_SOURCE_DIR}/include")
    target_compile_definitions(rasm_c_tests PRIVATE RASM_TEST_GOLDEN_DIR="${PROJECT_SOURCE_DIR}/tests/golden/lavapipe"
                                                    RASM_TEST_C_LIBRARY_NAME="$<TARGET_FILE_NAME:rasm_c>")
    rasm_copy_runtime_dlls(rasm_c_tests)
    add_test(NAME rasm_c_tests COMMAND rasm_c_tests)

    foreach (rasm_c_standard 99 11)
        set(rasm_target rasm_c_header_c${rasm_c_standard})
        add_executable(${rasm_target} "cApiHeaderCheck.c")
        target_link_libraries(${rasm_target} PRIVATE rasm::c_renderer)
        set_target_properties(${rasm_target} PROPERTIES C_STANDARD ${rasm_c_standard} C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
        target_compile_options(${rasm_target} PRIVATE -Wall -Wextra -pedantic -Werror)
        rasm_copy_runtime_dlls(${rasm_target})
        add_test(NAME ${rasm_target} COMMAND ${rasm_target})
    endforeach()
endif()

# Golden-image tests: render scenes headless and compare them with tests/golden/lavapipe. The goldens are lavapipe's,
# so tools/image_tests.py runs them on that driver; the presets' ctest leaves out the label "image".
add_executable(rasm_image_tests "image/imageTests.cpp" "image/allocationCounter.cpp")
target_link_libraries(rasm_image_tests PRIVATE rasm::rasm)
target_compile_definitions(rasm_image_tests PRIVATE RASM_IMAGE_TESTS_ASSET_ROOT="${PROJECT_SOURCE_DIR}/assets"
                                                    RASM_IMAGE_TESTS_SHADER_DIR="${PROJECT_SOURCE_DIR}/shaders"
                                                    RASM_IMAGE_TESTS_GOLDEN_DIR="${PROJECT_SOURCE_DIR}/tests/golden/lavapipe"
                                                    RASM_IMAGE_TESTS_OUT_DIR="${CMAKE_BINARY_DIR}/image-tests"
                                                    RASM_IMAGE_TESTS_SPONZA="${PROJECT_SOURCE_DIR}/build/test-assets/Sponza/glTF/Sponza.gltf")
rasm_copy_runtime_dlls(rasm_image_tests)

add_test(NAME rasm_image_tests COMMAND rasm_image_tests)
set_tests_properties(rasm_image_tests PROPERTIES LABELS image)
