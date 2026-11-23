include(RasmRuntimeDlls)

find_package(doctest CONFIG REQUIRED)
find_package(Ktx CONFIG REQUIRED)

add_executable(rasm_tests
            "main.cpp"
            "animationTests.cpp"
            "blendSortTests.cpp"
            "bloomTests.cpp"
            "cacheFileTests.cpp"
            "cameraTests.cpp"
            "environmentTests.cpp"
            "gltfImportTests.cpp"
            "gpuTableTests.cpp"
            "handleTests.cpp"
            "importMeshText.cpp"
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
            "renderGraphTests.cpp"
            "resourceDescTests.cpp"
            "shaderCacheTests.cpp"
            "shadowCascadesTests.cpp"
            "skinningTests.cpp"
            "taaTests.cpp"
            "textureStatesTests.cpp"
            "textureUploadTests.cpp"
            "transformTests.cpp"
            "uiTests.cpp"
            "uploadManagerTests.cpp"
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
