include(RasmRuntimeDlls)

find_package(doctest CONFIG REQUIRED)

add_executable(rasm_tests
            "main.cpp"
            "cameraTests.cpp"
            "gltfImportTests.cpp"
            "gpuTableTests.cpp"
            "handleTests.cpp"
            "importMeshText.cpp"
            "meshTests.cpp"
            "pathTests.cpp"
            "renderGraphTests.cpp"
            "resourceDescTests.cpp"
            "transformTests.cpp"
            "uploadManagerTests.cpp"
    )

target_link_libraries(rasm_tests PRIVATE rasm::rasm doctest::doctest)
# Private classes such as rhi::UploadManager are tested against a fake rhi::Device.
target_include_directories(rasm_tests PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_compile_definitions(rasm_tests PRIVATE RASM_TEST_EXECUTABLE_DIR="$<TARGET_FILE_DIR:rasm_tests>"
                                              RASM_TEST_DEFAULT_SHADER_DIR="${RASM_SHADER_DIR}")
rasm_copy_runtime_dlls(rasm_tests)

add_test(NAME rasm_tests COMMAND rasm_tests)
